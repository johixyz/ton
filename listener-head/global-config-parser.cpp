// global-config-parser.cpp

#include "td/utils/JsonBuilder.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Status.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include "adnl/adnl-node-id.hpp"
#include "overlay/overlay-id.hpp"

#include <string>
#include <vector>

namespace ton {
namespace listener {

// Структура для хранения информации о статическом узле из конфигурации
struct StaticNodeInfo {
  adnl::AdnlNodeIdFull id_full;
  adnl::AdnlNodeIdShort id_short;
  td::IPAddress addr;

  std::string to_string() const {
    return "StaticNode: " + id_short.bits256_value().to_hex() + " at " + addr.get_ip_str().str() + ":" +
           std::to_string(addr.get_port());
  }
};

// Класс для парсинга глобальной конфигурации TON
class GlobalConfigParser {
 public:
  // Парсинг JSON конфигурации
  static td::Result<std::vector<StaticNodeInfo>> parse_static_nodes(td::Slice json_data) {
    std::vector<StaticNodeInfo> result;

    auto json_parse_result = td::json_decode(json_data);
    if (json_parse_result.is_error()) {
      return json_parse_result.move_as_error();
    }

    auto json_value = json_parse_result.move_as_ok();
    if (json_value.type() != td::JsonValue::Type::Object) {
      return td::Status::Error("Expected JSON object in global config");
    }

    // Получаем раздел DHT
    auto& json_obj = json_value.get_object();
    auto dht_it = json_obj.find("dht");
    if (dht_it == json_obj.end() || dht_it->second.type() != td::JsonValue::Type::Object) {
      return td::Status::Error("No valid 'dht' section in global config");
    }

    auto& dht_obj = dht_it->second.get_object();

    // Получаем раздел static_nodes
    auto static_nodes_it = dht_obj.find("static_nodes");
    if (static_nodes_it == dht_obj.end() || static_nodes_it->second.type() != td::JsonValue::Type::Object) {
      return td::Status::Error("No valid 'static_nodes' section in DHT config");
    }

    auto& static_nodes_obj = static_nodes_it->second.get_object();

    // Получаем список нод
    auto nodes_it = static_nodes_obj.find("nodes");
    if (nodes_it == static_nodes_obj.end() || nodes_it->second.type() != td::JsonValue::Type::Array) {
      return td::Status::Error("No valid 'nodes' array in static_nodes");
    }

    auto& nodes_array = nodes_it->second.get_array();

    // Перебираем все ноды
    for (auto& node_value : nodes_array) {
      if (node_value.type() != td::JsonValue::Type::Object) {
        LOG(WARNING) << "Invalid node in static_nodes";
        continue;
      }

      auto& node_obj = node_value.get_object();

      // Получаем id узла
      auto id_it = node_obj.find("id");
      if (id_it == node_obj.end() || id_it->second.type() != td::JsonValue::Type::Object) {
        LOG(WARNING) << "Invalid node id in static_nodes";
        continue;
      }

      auto& id_obj = id_it->second.get_object();

      // Проверяем тип ключа
      auto type_it = id_obj.find("@type");
      if (type_it == id_obj.end() || type_it->second.type() != td::JsonValue::Type::String ||
          type_it->second.get_string() != "pub.ed25519") {
        LOG(WARNING) << "Unsupported key type in static_nodes";
        continue;
      }

      // Получаем ключ
      auto key_it = id_obj.find("key");
      if (key_it == id_obj.end() || key_it->second.type() != td::JsonValue::Type::String) {
        LOG(WARNING) << "Invalid key in static_nodes";
        continue;
      }

      std::string key_base64 = key_it->second.get_string().str();
      auto key_r = td::base64_decode(key_base64);
      if (key_r.is_error()) {
        LOG(WARNING) << "Invalid base64 key in static_nodes: " << key_r.error();
        continue;
      }

      auto key_data = key_r.move_as_ok();
      if (key_data.size() != 32) {
        LOG(WARNING) << "Invalid key size in static_nodes";
        continue;
      }

      // Создаем полный ID
      td::Bits256 key_bits;
      key_bits.as_slice().copy_from(key_data);
      auto pubkey_ed25519 = ton::pubkeys::Ed25519{key_bits};
      auto pub_key = ton::PublicKey{pubkey_ed25519};
      auto full_id = adnl::AdnlNodeIdFull{pub_key};
      auto short_id = full_id.compute_short_id();

      // Получаем список адресов
      auto addr_list_it = node_obj.find("addr_list");
      if (addr_list_it == node_obj.end() || addr_list_it->second.type() != td::JsonValue::Type::Object) {
        LOG(WARNING) << "Invalid addr_list in static_nodes";
        continue;
      }

      auto& addr_list_obj = addr_list_it->second.get_object();

      // Получаем массив адресов
      auto addrs_it = addr_list_obj.find("addrs");
      if (addrs_it == addr_list_obj.end() || addrs_it->second.type() != td::JsonValue::Type::Array) {
        LOG(WARNING) << "Invalid addrs array in static_nodes";
        continue;
      }

      auto& addrs_array = addrs_it->second.get_array();
      if (addrs_array.empty()) {
        LOG(WARNING) << "Empty addrs array in static_nodes";
        continue;
      }

      // Берем первый адрес
      auto& addr_value = addrs_array[0];
      if (addr_value.type() != td::JsonValue::Type::Object) {
        LOG(WARNING) << "Invalid addr in static_nodes";
        continue;
      }

      auto& addr_obj = addr_value.get_object();

      // Проверяем тип адреса
      auto addr_type_it = addr_obj.find("@type");
      if (addr_type_it == addr_obj.end() || addr_type_it->second.type() != td::JsonValue::Type::String ||
          addr_type_it->second.get_string() != "adnl.address.udp") {
        LOG(WARNING) << "Unsupported address type in static_nodes";
        continue;
      }

      // Получаем IP и порт
      auto ip_it = addr_obj.find("ip");
      auto port_it = addr_obj.find("port");

      if (ip_it == addr_obj.end() || ip_it->second.type() != td::JsonValue::Type::Number) {
        LOG(WARNING) << "Invalid IP in static_nodes";
        continue;
      }

      if (port_it == addr_obj.end() || port_it->second.type() != td::JsonValue::Type::Number) {
        LOG(WARNING) << "Invalid port in static_nodes";
        continue;
      }

      td::int32 ip = static_cast<td::int32>(td::to_integer<td::int32>(ip_it->second.get_number()));
      td::uint16 port = static_cast<td::uint16>(td::to_integer<td::int32>(port_it->second.get_number()));

      // Создаем адрес
      td::IPAddress addr;
      if (addr.init_ipv4_port(td::IPAddress::ipv4_to_str(ip), port).is_error()) {
        LOG(WARNING) << "Failed to initialize IP address";
        continue;
      }

      // Добавляем узел в результат
      result.push_back({full_id, short_id, addr});
    }

    return result;
  }

  // Извлечение overlay ID для мастерчейна и базового воркчейна
  static std::vector<overlay::OverlayIdFull> extract_default_overlay_ids() {
    std::vector<overlay::OverlayIdFull> result;

    // Создаем overlay ID для мастерчейна (блоки)
    {
      auto masterchain_blocks = create_tl_object<ton_api::tonNode_blockIdExt>(
          ton::masterchainId, ton::shardIdAll, 0, td::Bits256::zero(), td::Bits256::zero());
      auto node_id_full = adnl::AdnlNodeIdFull{ton::PublicKey{ton::pubkeys::Ed25519{masterchain_blocks->root_hash_}}};
      auto overlay_id = overlay::OverlayIdFull{node_id_full.pubkey().export_as_slice()};
      result.push_back(overlay_id);
    }

    // Создаем overlay ID для базового воркчейна (блоки)
    {
      auto basechain_blocks = create_tl_object<ton_api::tonNode_blockIdExt>(
          ton::basechainId, ton::shardIdAll, 0, td::Bits256::zero(), td::Bits256::zero());
      auto node_id_full = adnl::AdnlNodeIdFull{ton::PublicKey{ton::pubkeys::Ed25519{masterchain_blocks->root_hash_}}};
      auto overlay_id = overlay::OverlayIdFull{node_id_full.pubkey().export_as_slice()};
      result.push_back(overlay_id);
    }

    return result;
  }

  // Извлечение литсерверов (опционально для дополнительных соединений)
  static td::Result<std::vector<StaticNodeInfo>> parse_liteservers(td::Slice json_data) {
    std::vector<StaticNodeInfo> result;

    auto json_parse_result = td::json_decode(json_data);
    if (json_parse_result.is_error()) {
      return json_parse_result.move_as_error();
    }

    auto json_value = json_parse_result.move_as_ok();
    if (json_value.type() != td::JsonValue::Type::Object) {
      return td::Status::Error("Expected JSON object in global config");
    }

    // Получаем раздел liteservers
    auto& json_obj = json_value.get_object();
    auto liteservers_it = json_obj.find("liteservers");
    if (liteservers_it == json_obj.end() || liteservers_it->second.type() != td::JsonValue::Type::Array) {
      return td::Status::Error("No valid 'liteservers' section in global config");
    }

    auto& liteservers_array = liteservers_it->second.get_array();

    // Перебираем все лайтсерверы
    for (auto& server_value : liteservers_array) {
      if (server_value.type() != td::JsonValue::Type::Object) {
        LOG(WARNING) << "Invalid liteserver in config";
        continue;
      }

      auto& server_obj = server_value.get_object();

      // Получаем IP и порт
      auto ip_it = server_obj.find("ip");
      auto port_it = server_obj.find("port");

      if (ip_it == server_obj.end() || ip_it->second.type() != td::JsonValue::Type::Number) {
        LOG(WARNING) << "Invalid IP in liteserver";
        continue;
      }

      if (port_it == server_obj.end() || port_it->second.type() != td::JsonValue::Type::Number) {
        LOG(WARNING) << "Invalid port in liteserver";
        continue;
      }

      td::int32 ip = static_cast<td::int32>(td::to_integer<td::int32>(ip_it->second.get_number()));
      td::uint16 port = static_cast<td::uint16>(td::to_integer<td::int32>(port_it->second.get_number()));

      // Получаем id узла
      auto id_it = server_obj.find("id");
      if (id_it == server_obj.end() || id_it->second.type() != td::JsonValue::Type::Object) {
        LOG(WARNING) << "Invalid liteserver id";
        continue;
      }

      auto& id_obj = id_it->second.get_object();

      // Проверяем тип ключа
      auto type_it = id_obj.find("@type");
      if (type_it == id_obj.end() || type_it->second.type() != td::JsonValue::Type::String ||
          type_it->second.get_string() != "pub.ed25519") {
        LOG(WARNING) << "Unsupported liteserver key type";
        continue;
      }

      // Получаем ключ
      auto key_it = id_obj.find("key");
      if (key_it == id_obj.end() || key_it->second.type() != td::JsonValue::Type::String) {
        LOG(WARNING) << "Invalid liteserver key";
        continue;
      }

      std::string key_base64 = key_it->second.get_string().str();
      auto key_r = td::base64_decode(key_base64);
      if (key_r.is_error()) {
        LOG(WARNING) << "Invalid base64 liteserver key: " << key_r.error();
        continue;
      }

      auto key_data = key_r.move_as_ok();
      if (key_data.size() != 32) {
        LOG(WARNING) << "Invalid liteserver key size";
        continue;
      }

      // Создаем полный ID
      td::Bits256 key_bits;
      key_bits.as_slice().copy_from(key_data);
      auto pubkey_ed25519 = ton::pubkeys::Ed25519{key_bits};
      auto pub_key = ton::PublicKey{pubkey_ed25519};
      auto full_id = adnl::AdnlNodeIdFull{pub_key};
      auto short_id = full_id.compute_short_id();

      // Создаем адрес
      td::IPAddress addr;
      if (addr.init_ipv4_port(td::IPAddress::ipv4_to_str(ip), port).is_error()) {
        LOG(WARNING) << "Failed to initialize liteserver IP address";
        continue;
      }

      // Добавляем узел в результат
      result.push_back({full_id, short_id, addr});
    }

    return result;
  }
};

} // namespace listener
} // namespace ton
