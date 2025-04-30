#pragma once

#include "td/utils/json.h"
#include "adnl/adnl-node-id.hpp"
#include "td/utils/port/IPAddress.h"
#include <vector>
#include <string>

namespace ton {
namespace listener {

// Структура для хранения информации о валидаторе
struct ValidatorInfo {
  adnl::AdnlNodeIdShort id;
  td::IPAddress addr;

  static ValidatorInfo from_json(const td::JsonObject &obj) {
    ValidatorInfo info;

    auto id_str = td::get_json_object_string_field(obj.get_object(), "id", "");
    auto addr_str = td::get_json_object_string_field(obj.get_object(), "addr", "");
    auto port = td::get_json_object_int_field(obj.get_object(), "port", 0);

    // Parse ADNL node ID
    if (!id_str.empty()) {
      info.id = adnl::AdnlNodeIdShort::parse(id_str).move_as_ok();
    }

    // Parse IP address
    if (!addr_str.empty() && port > 0) {
      info.addr.init_host_port(addr_str, port).ensure();
    }

    return info;
  }
};

// Структура для конфигурации оверлея
struct OverlayInfo {
  std::string name;
  std::string overlay_id;

  static OverlayInfo from_json(const td::JsonObject &obj) {
    OverlayInfo info;

    info.name = td::get_json_object_string_field(obj.get_object(), "name", "");
    info.overlay_id = td::get_json_object_string_field(obj.get_object(), "overlay_id", "");

    return info;
  }
};

// Главная структура конфигурации
struct ListenerConfig {
  // Настройки Listener Head
  int max_connections = 1000;
  int udp_buffer_size = 10 * 1024 * 1024;
  int http_port = 8080;
  int log_level = 3;

  static ListenerConfig from_json(const td::JsonValue &json) {
    ListenerConfig config;

    if (json.type() != td::JsonValue::Type::Object) {
      return config;
    }

    auto &obj = json.get_object();
    auto listener = td::get_json_object_field(obj, "listener", td::JsonValue::Type::Object, td::JsonValue());

    if (listener.type() == td::JsonValue::Type::Object) {
      auto &listener_obj = listener.get_object();
      config.max_connections = td::get_json_object_int_field(listener_obj, "max_connections", 1000);
      config.udp_buffer_size = td::get_json_object_int_field(listener_obj, "udp_buffer_size", 10 * 1024 * 1024);
      config.http_port = td::get_json_object_int_field(listener_obj, "http_port", 8080);
      config.log_level = td::get_json_object_int_field(listener_obj, "log_level", 3);
    }

    return config;
  }
};

} // namespace listener
} // namespace ton
