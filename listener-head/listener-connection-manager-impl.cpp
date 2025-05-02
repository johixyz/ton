// listener-connection-manager-impl.cpp

#include "listener-connection-manager.hpp"
#include "td/utils/overloaded.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"

namespace ton {
namespace listener {

void ListenerConnectionManager::discover_overlay_peers(overlay::OverlayIdShort overlay_id) {
  LOG(INFO) << "Discovering peers for overlay " << overlay_id.bits256_value();

  // Создаем callback для обработки результатов поиска пиров
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), overlay_id](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
    if (R.is_error()) {
      LOG(WARNING) << "Failed to get overlay peers: " << R.move_as_error();
      return;
    }

    auto peers = R.move_as_ok();
    LOG(INFO) << "Discovered " << peers.size() << " peers for overlay " << overlay_id.bits256_value();

    // Добавляем найденные пиры
    for (auto& peer_id : peers) {
      td::actor::send_closure(SelfId, &ListenerConnectionManager::lookup_peer_address, peer_id);
    }
  });

  // Запрашиваем случайных пиров из оверлея
  td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers,
                          local_id_, overlay_id, 50, std::move(P));
}

void ListenerConnectionManager::lookup_peer_address(adnl::AdnlNodeIdShort peer_id) {
  LOG(INFO) << "Looking up address for peer " << peer_id.bits256_value();

  // Создаем DHT ключ для этого ID узла
  auto key = peer_id.pubkey_hash();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), peer_id](td::Result<dht::DhtValue> R) {
    if (R.is_error()) {
      LOG(WARNING) << "Failed to lookup peer address: " << R.move_as_error();
      return;
    }

    auto dht_value = R.move_as_ok();
    auto R2 = fetch_tl_object<ton_api::adnl_addressList>(dht_value.value().clone(), true);
    if (R2.is_error()) {
      LOG(WARNING) << "Invalid address list format: " << R2.move_as_error();
      return;
    }

    auto addr_list = R2.move_as_ok();

    // Преобразуем в IP адрес и добавляем пир
    for (auto& addr : addr_list->addrs_) {
      if (addr->get_id() == ton_api::adnl_address_udp::ID) {
        auto& udp_addr = static_cast<const ton_api::adnl_address_udp&>(*addr);

        td::IPAddress ip_addr;
        auto ip = udp_addr.ip_;
        auto port = udp_addr.port_;

        if (ip_addr.init_ipv4_port(td::IPAddress::ipv4_to_str(ip), port).is_ok()) {
          td::actor::send_closure(SelfId, &ListenerConnectionManager::add_peer, peer_id, ip_addr, false);
          break;
        }
      }
    }
  });

  // Запрашиваем адрес пира из DHT
  td::actor::send_closure(dht_, &dht::Dht::get_value, dht::DhtKey{key, "address", 0}, std::move(P));
}

void ListenerConnectionManager::discover_new_peers() {
  if (overlays_to_monitor_.empty()) {
    return;
  }

  LOG(DEBUG) << "Discovering new peers...";

  // Выбираем случайный оверлей для поиска пиров
  size_t index = rand() % overlays_to_monitor_.size();
  auto it = overlays_to_monitor_.begin();
  std::advance(it, index);
  auto overlay_id = *it;

  LOG(INFO) << "Looking for peers in overlay " << overlay_id.bits256_value();
  discover_overlay_peers(overlay_id);

  // Для каждого оверлея задаем также вероятность поиска через DHT
  if (rand() % 100 < 20) { // 20% вероятность поиска через DHT
    lookup_peers_via_dht(overlay_id);
  }
}

void ListenerConnectionManager::lookup_peers_via_dht(overlay::OverlayIdShort overlay_id) {
  LOG(INFO) << "Looking up peers via DHT for overlay " << overlay_id.bits256_value();

  // Создаем DHT ключ для этого оверлея (узлы оверлея)
  dht::DhtKey dht_key{overlay_id.pubkey_hash(), "nodes", 0};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<dht::DhtValue> R) {
    if (R.is_error()) {
      LOG(WARNING) << "DHT lookup failed: " << R.move_as_error();
      return;
    }

    auto dht_value = R.move_as_ok();
    auto R2 = fetch_tl_object<ton_api::overlay_nodes>(dht_value.value().clone(), true);
    if (R2.is_error()) {
      LOG(WARNING) << "Invalid overlay_nodes format: " << R2.move_as_error();
      return;
    }

    auto nodes = R2.move_as_ok();

    LOG(INFO) << "Found " << nodes->nodes_.size() << " nodes via DHT";

    // Обрабатываем найденные узлы
    for (auto& node : nodes->nodes_) {
      if (!node) {
        continue;
      }

      // Извлекаем ID узла
      auto R3 = adnl::AdnlNodeIdFull::create(node->id_);
      if (R3.is_error()) {
        LOG(WARNING) << "Invalid node id: " << R3.move_as_error();
        continue;
      }

      auto id_full = R3.move_as_ok();
      auto id_short = id_full.compute_short_id();

      // Пытаемся извлечь IP адрес
      auto R4 = adnl::AdnlAddressList::create(std::move(node->addr_list_));
      if (R4.is_error()) {
        LOG(WARNING) << "Invalid address list: " << R4.move_as_error();
        continue;
      }

      auto addr_list = R4.move_as_ok();
      auto addr = addr_list.get_udp_address();

      if (addr.is_valid()) {
        // Добавляем пир
        td::actor::send_closure(SelfId, &ListenerConnectionManager::add_peer, id_short, addr, false);
      }
    }
  });

  // Запрашиваем узлы оверлея из DHT
  td::actor::send_closure(dht_, &dht::Dht::get_value, std::move(dht_key), std::move(P));
}

void ListenerConnectionManager::connect_to_peer(adnl::AdnlNodeIdShort peer_id, td::IPAddress addr, bool is_validator) {
  LOG(INFO) << "Connecting to peer: " << peer_id.bits256_value() << " at " << addr.get_ip_str()
            << (is_validator ? " (validator)" : "");

  td::uint32 priority = is_validator ? NetworkConfig::VALIDATOR_PRIORITY : 0;

  // Создаем список адресов для ADNL
  adnl::AdnlAddressList addr_list;
  addr_list.add_udp_address(addr);

  // Создаем полный идентификатор узла
  // Это упрощенная реализация - в идеале нужно получить полный ключ через DHT
  auto pubkey = ton::PublicKey(ton::pubkeys::Ed25519{peer_id.bits256_value()});
  auto full_id = ton::adnl::AdnlNodeIdFull{pubkey};

  // Добавляем пир в ADNL
  td::actor::send_closure(adnl_, &adnl::Adnl::add_peer, local_id_, peer_id, full_id, addr_list, priority);

  // Создаем запрос на пинг для проверки соединения
  create_ping_query(peer_id);
}

void ListenerConnectionManager::create_ping_query(adnl::AdnlNodeIdShort peer_id) {
  auto query = create_tl_object<ton_api::adnl_ping>(td::Random::fast_uint64());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), peer_id](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      LOG(WARNING) << "Ping to " << peer_id.bits256_value() << " failed: " << R.move_as_error();
      td::actor::send_closure(SelfId, &ListenerConnectionManager::update_peer_status, peer_id, false);
    } else {
      LOG(DEBUG) << "Received pong from " << peer_id.bits256_value();
      td::actor::send_closure(SelfId, &ListenerConnectionManager::update_peer_status, peer_id, true);
    }
  });

  // Отправляем пинг через ADNL
  td::actor::send_closure(adnl_, &adnl::AdnlSenderInterface::send_query, local_id_, peer_id, "", std::move(P),
                          td::Timestamp::in(2.0), serialize_tl_object(query, true), td::rand_fast() % 65536);
}

void ListenerConnectionManager::update_peer_status(adnl::AdnlNodeIdShort peer_id, bool success) {
  auto it = peers_.find(peer_id);
  if (it != peers_.end()) {
    it->second.last_connect_attempt = td::Timestamp::now();

    if (success) {
      it->second.ping_success_count++;
      it->second.ping_fail_count = 0;

      // Если узел стабильно отвечает на пинги, считаем его валидатором
      if (it->second.ping_success_count > 5 && !it->second.is_validator) {
        LOG(INFO) << "Promoting peer " << peer_id.bits256_value() << " to validator due to stable connection";
        it->second.is_validator = true;
      }
    } else {
      it->second.ping_fail_count++;
      it->second.ping_success_count = 0;

      // Если узел не отвечает на несколько пингов подряд, понижаем его приоритет
      if (it->second.ping_fail_count > 3 && it->second.is_validator) {
        LOG(WARNING) << "Demoting validator " << peer_id.bits256_value() << " due to connection failures";
        it->second.is_validator = false;
      }
    }
  }
}

void ListenerConnectionManager::update_connections() {
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

  // Очистка неактивных пиров
  gc_inactive_peers();
}

void ListenerConnectionManager::gc_inactive_peers() {
  auto now = td::Timestamp::now();
  std::vector<adnl::AdnlNodeIdShort> to_remove;

  for (auto& p : peers_) {
    // Если пир не отвечал на пинги более часа и не является валидатором
    if (!p.second.is_validator &&
        p.second.ping_fail_count > 5 &&
        now.at() - p.second.last_connect_attempt.at() > 3600.0) {
      to_remove.push_back(p.first);
    }
  }

  for (auto& id : to_remove) {
    LOG(INFO) << "Removing inactive peer " << id.bits256_value();
    peers_.erase(id);
  }
}

void ListenerConnectionManager::start_up() {
  LOG(INFO) << "ListenerConnectionManager starting up...";
  alarm_timestamp() = td::Timestamp::in(1.0);
}

void ListenerConnectionManager::alarm() {
  update_connections();
  discover_new_peers();
  alarm_timestamp() = td::Timestamp::in(60.0); // Проверка каждую минуту
}

} // namespace listener
} // namespace ton
