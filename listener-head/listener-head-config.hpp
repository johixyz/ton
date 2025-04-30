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
  std::vector<ValidatorInfo> validators;
  std::vector<ValidatorInfo> seed_nodes;
  std::vector<OverlayInfo> overlays;

  // Настройки Listener Head
  int max_connections = 1000;
  int udp_buffer_size = 10 * 1024 * 1024;
  int http_port = 8080;

  static ListenerConfig from_json(const td::JsonValue &json) {
    ListenerConfig config;

    if (json.type() != td::JsonValue::Type::Object) {
      return config;
    }

    auto &obj = json.get_object();

    // Загрузка валидаторов
    auto validators = td::get_json_object_field(obj, "validators", td::JsonValue::Type::Array, td::JsonValue());
    if (validators.type() == td::JsonValue::Type::Array) {
      for (auto &val : validators.get_array()) {
        if (val.type() == td::JsonValue::Type::Object) {
          config.validators.push_back(ValidatorInfo::from_json(val));
        }
      }
    }

    // Загрузка seed-нод
    auto seeds = td::get_json_object_field(obj, "seed_nodes", td::JsonValue::Type::Array, td::JsonValue());
    if (seeds.type() == td::JsonValue::Type::Array) {
      for (auto &seed : seeds.get_array()) {
        if (seed.type() == td::JsonValue::Type::Object) {
          config.seed_nodes.push_back(ValidatorInfo::from_json(seed));
        }
      }
    }

    // Загрузка оверлеев
    auto overlays = td::get_json_object_field(obj, "overlays", td::JsonValue::Type::Array, td::JsonValue());
    if (overlays.type() == td::JsonValue::Type::Array) {
      for (auto &overlay : overlays.get_array()) {
        if (overlay.type() == td::JsonValue::Type::Object) {
          config.overlays.push_back(OverlayInfo::from_json(overlay));
        }
      }
    }

    // Загрузка настроек
    config.max_connections = td::get_json_object_int_field(obj, "max_connections", 1000);
    config.udp_buffer_size = td::get_json_object_int_field(obj, "udp_buffer_size", 10 * 1024 * 1024);
    config.http_port = td::get_json_object_int_field(obj, "http_port", 8080);

    return config;
  }
};

} // namespace listener
} // namespace ton
