#pragma once

#include "td/actor/actor.h"
#include "adnl/adnl.h"
#include "adnl/adnl-peer-table.h"
#include "overlay/overlays.h"
#include "dht/dht.h"
#include "td/actor/actor.h"
#include <set>
#include <map>

namespace ton {
namespace listener {

// Константы для настройки сети
struct NetworkConfig {
  static constexpr size_t MAX_OUTBOUND_CONNECTIONS = 1000;
  static constexpr double CONNECTION_RESET_INTERVAL = 600.0; // 10 минут
  static constexpr td::uint32 VALIDATOR_PRIORITY = 10;       // Приоритет для валидаторов
};

// Менеджер соединений - отвечает за установку и поддержание соединений
class ListenerConnectionManager : public td::actor::Actor {
 public:
  ListenerConnectionManager(td::actor::ActorId<adnl::Adnl> adnl,
                            td::actor::ActorId<overlay::Overlays> overlays,
                            td::actor::ActorId<dht::Dht> dht)
      : adnl_(adnl), overlays_(overlays), dht_(dht) {
  }

  // Добавление пира для подключения
  void add_peer(adnl::AdnlNodeIdShort peer_id, td::IPAddress addr, bool is_validator = false) {
    LOG(INFO) << "Adding peer " << peer_id.to_hex() << " at " << addr.get_ip_str()
              << (is_validator ? " (validator)" : "");

    peers_[peer_id] = {addr, is_validator, td::Timestamp::now()};
    if (is_validator) {
      validators_.insert(peer_id);
    }
    update_connections();
  }

  // Добавление оверлея для мониторинга и поиска пиров
  void add_overlay(overlay::OverlayIdShort overlay_id) {
    LOG(INFO) << "Adding overlay to monitor: " << overlay_id.to_hex();

    overlays_to_monitor_.insert(overlay_id);
    discover_overlay_peers(overlay_id);
  }

  // Установка максимального числа исходящих соединений
  void set_max_connections(size_t max_connections) {
    LOG(INFO) << "Setting max connections to " << max_connections;
    max_connections_ = max_connections;
  }

  // Получение списка активных соединений
  std::vector<std::pair<adnl::AdnlNodeIdShort, td::IPAddress>> get_active_connections() const {
    std::vector<std::pair<adnl::AdnlNodeIdShort, td::IPAddress>> result;
    for (const auto& p : peers_) {
      result.emplace_back(p.first, p.second.addr);
    }
    return result;
  }

  // Получение статистики соединений
  std::string get_connections_json() const {
    std::string result = "{\n  \"connections\": [\n";

    size_t i = 0;
    for (const auto& p : peers_) {
      result += "    {\n";
      result += "      \"peer_id\": \"" + p.first.to_hex() + "\",\n";
      result += "      \"ip\": \"" + p.second.addr.get_ip_str().str() + "\",\n";
      result += "      \"port\": " + std::to_string(p.second.addr.get_port()) + ",\n";
      result += "      \"is_validator\": " + std::string(p.second.is_validator ? "true" : "false") + ",\n";
      result += "      \"last_connect\": " + std::to_string(p.second.last_connect_attempt.at()) + "\n";
      result += "    }";
      if (++i < peers_.size()) {
        result += ",";
      }
      result += "\n";
    }

    result += "  ],\n";
    result += "  \"total_connections\": " + std::to_string(peers_.size()) + ",\n";
    result += "  \"validator_connections\": " + std::to_string(validators_.size()) + ",\n";
    result += "  \"max_connections\": " + std::to_string(max_connections_) + "\n";
    result += "}\n";

    return result;
  }

  // Методы жизненного цикла актора
  void start_up() override {
    LOG(INFO) << "ListenerConnectionManager starting up...";
    alarm_timestamp() = td::Timestamp::in(1.0);
  }

  void alarm() override {
    update_connections();
    discover_new_peers();
    alarm_timestamp() = td::Timestamp::in(60.0); // Проверка каждую минуту
  }

 private:
  struct PeerInfo {
    td::IPAddress addr;
    bool is_validator;
    td::Timestamp last_connect_attempt;

    PeerInfo() : is_validator(false) {}
    PeerInfo(td::IPAddress addr, bool is_validator, td::Timestamp last_connect_attempt)
        : addr(addr), is_validator(is_validator), last_connect_attempt(last_connect_attempt) {}
  };

  // Обновление соединений - подключаемся к пирам согласно приоритетам
  void update_connections() {
    LOG(DEBUG) << "Updating connections...";

    // Сначала подключаемся к валидаторам
    for (auto& id : validators_) {
      auto it = peers_.find(id);
      if (it != peers_.end() && (td::Timestamp::now().at() - it->second.last_connect_attempt.at() >
                                 NetworkConfig::CONNECTION_RESET_INTERVAL)) {
        connect_to_peer(id, it->second.addr, true);
        it->second.last_connect_attempt = td::Timestamp::now();
      }
    }

    // Затем к остальным узлам
    size_t current_connections = validators_.size();
    for (auto& p : peers_) {
      if (validators_.count(p.first) > 0) {
        continue;
      }

      if (current_connections >= max_connections_) {
        break;
      }

      if (td::Timestamp::now().at() - p.second.last_connect_attempt.at() >
          NetworkConfig::CONNECTION_RESET_INTERVAL) {
        connect_to_peer(p.first, p.second.addr, false);
        p.second.last_connect_attempt = td::Timestamp::now();
        current_connections++;
      }
    }
  }

  // Подключение к конкретному пиру
  void connect_to_peer(adnl::AdnlNodeIdShort peer_id, td::IPAddress addr, bool is_validator) {
    LOG(INFO) << "Connecting to peer " << peer_id.to_hex() << " at " << addr.get_ip_str()
              << (is_validator ? " (validator)" : "");

    td::uint32 priority = is_validator ? NetworkConfig::VALIDATOR_PRIORITY : 0;

    // Создаем список адресов для ADNL
    adnl::AdnlAddressList addr_list;
    addr_list.add_udp_address(addr);

    // Добавляем пир в ADNL
    td::actor::send_closure(adnl_, &adnl::Adnl::add_peer, peer_id, addr_list, priority);
  }

  // Поиск новых пиров через DHT для всех мониторимых оверлеев
  void discover_new_peers() {
    if (overlays_to_monitor_.empty()) {
      return;
    }

    LOG(DEBUG) << "Discovering new peers...";

    // Выбираем случайный оверлей для поиска пиров
    size_t index = rand() % overlays_to_monitor_.size();
    auto it = overlays_to_monitor_.begin();
    std::advance(it, index);
    auto overlay_id = *it;

    LOG(INFO) << "Looking for peers in overlay " << overlay_id.to_hex();
    discover_overlay_peers(overlay_id);
  }

  // Поиск пиров для конкретного оверлея
  void discover_overlay_peers(overlay::OverlayIdShort overlay_id) {
    LOG(INFO) << "Discovering peers for overlay " << overlay_id.to_hex();

    // Здесь должен быть код для поиска пиров через доступный API
    // Например, через DHT или другие механизмы TON

    // Примечание: конкретная реализация будет зависеть от доступных API
    // В вашей версии TON
  }

  // Поиск адреса узла через DHT или другие механизмы
  void lookup_peer_address(adnl::AdnlNodeIdShort peer_id) {
    LOG(INFO) << "Looking up address for peer " << peer_id.to_hex();

    // Здесь должен быть код для поиска адреса узла
    // через доступные API TON
  }

  // Поля класса
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<dht::Dht> dht_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  std::map<adnl::AdnlNodeIdShort, PeerInfo> peers_;
  std::set<adnl::AdnlNodeIdShort> validators_;
  std::set<overlay::OverlayIdShort> overlays_to_monitor_;

  size_t max_connections_ = NetworkConfig::MAX_OUTBOUND_CONNECTIONS;
};

} // namespace listener
} // namespace ton