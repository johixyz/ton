#pragma once

#include "td/utils/json.h"
#include <string>

namespace ton {
namespace listener {

// Упрощенная структура конфигурации
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