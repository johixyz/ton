#pragma once

#include "td/utils/JsonBuilder.h"
#include "td/utils/misc.h"
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

    // Извлекаем секцию "listener"
    auto listener_it = std::find_if(json.get_object().begin(), json.get_object().end(),
                                    [](const auto &kv) { return kv.first == "listener"; });

    if (listener_it != json.get_object().end() && listener_it->second.type() == td::JsonValue::Type::Object) {
      const auto &listener_obj = listener_it->second.get_object();

      // Извлекаем значения из конфигурации
      auto extract_int = [&](const std::string &field_name, int default_value) -> int {
        auto it = std::find_if(listener_obj.begin(), listener_obj.end(),
                               [&field_name](const auto &kv) { return kv.first == field_name; });
        if (it != listener_obj.end() && it->second.type() == td::JsonValue::Type::Number) {
          return td::to_integer<int>(it->second.get_number());
        }
        return default_value;
      };

      config.max_connections = extract_int("max_connections", 1000);
      config.udp_buffer_size = extract_int("udp_buffer_size", 10 * 1024 * 1024);
      config.http_port = extract_int("http_port", 8080);
      config.log_level = extract_int("log_level", 3);
    }

    return config;
  }
};

} // namespace listener
} // namespace ton