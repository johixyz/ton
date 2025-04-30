#pragma once

#include "adnl/adnl.h"
#include "adnl/adnl-peer-table.h"
#include "overlay/overlays.h"
#include "listener-network-config.hpp"
#include "td/actor/actor.h"
#include <set>
#include <map>

namespace ton {
namespace listener {

class ConnectionManager : public td::actor::Actor {
 public:
  ConnectionManager(td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<overlay::Overlays> overlays,
                    td::actor::ActorId<dht::Dht> dht)
      : adnl_(adnl), overlays_(overlays), dht_(dht) {
  }

  void add_peer(adnl::AdnlNodeIdShort peer_id, td::IPAddress addr, bool is_validator = false) {
    peers_[peer_id] = {addr, is_validator};
    if (is_validator) {
      validators_.insert(peer_id);
    }
    update_connections();
  }

  void add_overlay(overlay::OverlayIdShort overlay_id) {
    overlays_to_monitor_.insert(overlay_id);
    update_connections();
  }

  void set_max_connections(size_t max_connections) {
    max_connections_ = max_connections;
  }

 private:
  struct PeerInfo {
    td::IPAddress addr;
    bool is_validator;
    td::Timestamp last_connect_attempt;

    PeerInfo() : is_validator(false) {}
    PeerInfo(td::IPAddress addr, bool is_validator)
        : addr(addr), is_validator(is_validator), last_connect_attempt(td::Timestamp::now()) {}
  };

  void update_connections() {
    // Сначала подключаемся к валидаторам
    for (auto& id : validators_) {
      auto it = peers_.find(id);
      if (it != peers_.end() && (td::Timestamp::now().at() - it->second.last_connect_attempt.at() >
                                 NetworkOptimizationConfig::CONNECTION_RESET_INTERVAL)) {
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
          NetworkOptimizationConfig::CONNECTION_RESET_INTERVAL) {
        connect_to_peer(p.first, p.second.addr, false);
        p.second.last_connect_attempt = td::Timestamp::now();
        current_connections++;
      }
    }
  }

  void connect_to_peer(adnl::AdnlNodeIdShort peer_id, td::IPAddress addr, bool is_validator) {
    LOG(INFO) << "Connecting to peer " << peer_id.serialize() << " at " << addr
              << (is_validator ? " (validator)" : "");

    td::uint32 priority = is_validator ? NetworkOptimizationConfig::VALIDATOR_PRIORITY : 0;

    // Создаем список адресов для ADNL
    adnl::AdnlAddressList addr_list;
    addr_list.add_udp_address(addr);

    // Добавляем пир в ADNL
    td::actor::send_closure(adnl_, &adnl::Adnl::add_peer, peer_id, addr_list, priority);

    // Для оверлея также важно подключиться
    if (is_validator) {
      for (auto& overlay_id : overlays_to_monitor_) {
        // Используем subscribe вместо add_node
        td::actor::send_closure(overlays_, &overlay::Overlays::subscribe, overlay_id, peer_id);
      }
    }
  }

  void alarm() override {
    alarm_timestamp() = td::Timestamp::in(60.0);
    update_connections();
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(1.0);

    // Запускаем поиск в DHT
    if (!overlays_to_monitor_.empty()) {
      for (auto& overlay_id : overlays_to_monitor_) {
        LOG(INFO) << "Запрашиваем узлы для оверлея " << overlay_id.serialize();

        // Получаем случайных пиров через DHT
        td::actor::send_closure(dht_, &dht::Dht::get_peers, overlay_id,
                                10, [SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
                                  if (R.is_ok()) {
                                    auto peers = R.move_as_ok();
                                    for (const auto& peer_id : peers) {
                                      // У нас нет адреса, но можем попробовать подключиться через DHT
                                      td::actor::send_closure(SelfId, &ConnectionManager::attempt_peer_lookup, peer_id);
                                    }
                                  }
                                });
      }
    }
  }

  // Дополнительный метод для поиска адреса узла через DHT
  void attempt_peer_lookup(adnl::AdnlNodeIdShort peer_id) {
    td::actor::send_closure(dht_, &dht::Dht::get_address_list, peer_id,
                            [SelfId = actor_id(this), peer_id](td::Result<adnl::AdnlAddressList> R) {
                              if (R.is_ok()) {
                                auto addr_list = R.move_as_ok();
                                if (!addr_list.empty()) {
                                  auto addr = addr_list.get_udp_address(0);
                                  td::actor::send_closure(SelfId, &ConnectionManager::add_peer, peer_id, addr, false);
                                }
                              }
                            });
  }

  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<dht::Dht> dht_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  std::map<adnl::AdnlNodeIdShort, PeerInfo> peers_;
  std::set<adnl::AdnlNodeIdShort> validators_;
  std::set<overlay::OverlayIdShort> overlays_to_monitor_;

  size_t max_connections_ = NetworkOptimizationConfig::MAX_OUTBOUND_CONNECTIONS;
};

} // namespace listener
} // namespace ton