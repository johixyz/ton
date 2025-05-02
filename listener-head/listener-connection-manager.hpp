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
      : adnl_(adnl), dht_(dht), overlays_(overlays) {
  }

  // Установка локального ADNL ID
  void set_local_id(adnl::AdnlNodeIdShort local_id) {
    local_id_ = local_id;
  }

  // Добавление пира для подключения
  void add_peer(adnl::AdnlNodeIdShort peer_id, td::IPAddress addr, bool is_validator = false) {
    LOG(INFO) << "Adding peer " << peer_id.bits256_value()
              << " at " << addr.get_ip_str()
              << (is_validator ? " (validator)" : "");

    peers_[peer_id] = {addr, is_validator, td::Timestamp::now()};
    if (is_validator) {
      validators_.insert(peer_id);
    }
    update_connections();
  }

  // Добавление оверлея для мониторинга и поиска пиров
  void add_overlay(overlay::OverlayIdShort overlay_id) {
    LOG(INFO) << "Adding overlay to monitor: " << overlay_id.bits256_value();

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
      result += "      \"peer_id\": \"" + p.first.bits256_value().to_hex() + "\",\n";
      result += "      \"ip\": \"" + p.second.addr.get_ip_str().str() + "\",\n";
      result += "      \"port\": " + std::to_string(p.second.addr.get_port()) + ",\n";
      result += "      \"is_validator\": " + std::string(p.second.is_validator ? "true" : "false") + ",\n";
      result += "      \"last_connect\": " + std::to_string(p.second.last_connect_attempt.at()) + ",\n";
      result += "      \"ping_success\": " + std::to_string(p.second.ping_success_count) + ",\n";
      result += "      \"ping_fail\": " + std::to_string(p.second.ping_fail_count) + "\n";
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

  // Поиск пиров в DHT
  void discover_overlay_peers(overlay::OverlayIdShort overlay_id);

  // Поиск адреса узла через DHT
  void lookup_peer_address(adnl::AdnlNodeIdShort peer_id);

  // Поиск пиров через DHT для конкретного оверлея
  void lookup_peers_via_dht(overlay::OverlayIdShort overlay_id);

  // Обновление статуса пира (успешный или неуспешный пинг)
  void update_peer_status(adnl::AdnlNodeIdShort peer_id, bool success);

  // Создание пинг-запроса для проверки соединения
  void create_ping_query(adnl::AdnlNodeIdShort peer_id);

  // Очистка неактивных пиров
  void gc_inactive_peers();

  // Методы жизненного цикла актора
  void start_up() override;
  void alarm() override;

 private:
  struct PeerInfo {
    td::IPAddress addr;
    bool is_validator;
    td::Timestamp last_connect_attempt;
    td::uint32 ping_success_count = 0;
    td::uint32 ping_fail_count = 0;

    PeerInfo() : is_validator(false) {}
    PeerInfo(td::IPAddress addr, bool is_validator, td::Timestamp last_connect_attempt)
        : addr(addr), is_validator(is_validator), last_connect_attempt(last_connect_attempt) {}
  };

  // Обновление соединений - подключаемся к пирам согласно приоритетам
  void update_connections();

  // Подключение к конкретному пиру
  void connect_to_peer(adnl::AdnlNodeIdShort peer_id, td::IPAddress addr, bool is_validator);

  // Поиск новых пиров через DHT для всех мониторимых оверлеев
  void discover_new_peers();

  // Поля класса
  adnl::AdnlNodeIdShort local_id_;
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