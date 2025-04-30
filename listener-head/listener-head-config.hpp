#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/Status.h"
#include "td/utils/format.h"

namespace ton {
namespace listener {
// Конфигурация ListenerHead
struct ListenerHeadConfig {
  // Общие настройки
  td::uint16 http_port = 8080;
  int log_level = 3;
  int max_connections = 1000;
  int udp_buffer_size = 10 * 1024 * 1024;

  // Сетевые настройки
  std::vector<std::pair<std::string, std::string>> static_nodes; // пары (адрес, ключ)
  std::vector<std::string> overlay_ids;                          // оверлеи для мониторинга

  // Настройки хранения
  int max_blocks_to_store = 10000;
  bool save_blocks_to_db = false;

  // Загрузка из JSON
  static ListenerHeadConfig load_from_json(const std::string& json_data) {
    ListenerHeadConfig config;

    auto json_parse_result = td::json_decode(td::MutableSlice(const_cast<char*>(json_data.c_str()), json_data.size()));
    if (json_parse_result.is_error()) {
      LOG(ERROR) << "Error parsing JSON config: " << json_parse_result.error().message();
      return config;
    }

    auto json_value = json_parse_result.move_as_ok();
    if (json_value.type() != td::JsonValue::Type::Object) {
      LOG(ERROR) << "Expected JSON object in config";
      return config;
    }

    auto& root = json_value.get_object();

    // Parse http_port
    auto http_port_field = td::get_json_object_field(root, "http_port", td::JsonValue::Type::Number, false);
    if (http_port_field.is_ok()) {
      auto value = http_port_field.move_as_ok();
      config.http_port = static_cast<td::uint16>(td::to_integer<int>(value.get_number()));
    }

    // Parse log_level
    auto log_level_field = td::get_json_object_field(root, "log_level", td::JsonValue::Type::Number, false);
    if (log_level_field.is_ok()) {
      auto value = log_level_field.move_as_ok();
      config.log_level = td::to_integer<int>(value.get_number());
    }

    // Parse max_connections
    auto max_conn_field = td::get_json_object_field(root, "max_connections", td::JsonValue::Type::Number, false);
    if (max_conn_field.is_ok()) {
      auto value = max_conn_field.move_as_ok();
      config.max_connections = td::to_integer<int>(value.get_number());
    }

    // Parse udp_buffer_size
    auto udp_buffer_field = td::get_json_object_field(root, "udp_buffer_size", td::JsonValue::Type::Number, false);
    if (udp_buffer_field.is_ok()) {
      auto value = udp_buffer_field.move_as_ok();
      config.udp_buffer_size = td::to_integer<int>(value.get_number());
    }

    // Parse max_blocks_to_store
    auto max_blocks_field = td::get_json_object_field(root, "max_blocks_to_store", td::JsonValue::Type::Number, false);
    if (max_blocks_field.is_ok()) {
      auto value = max_blocks_field.move_as_ok();
      config.max_blocks_to_store = td::to_integer<int>(value.get_number());
    }

    // Parse save_blocks_to_db
    auto save_blocks_field = td::get_json_object_field(root, "save_blocks_to_db", td::JsonValue::Type::Boolean, false);
    if (save_blocks_field.is_ok()) {
      auto value = save_blocks_field.move_as_ok();
      config.save_blocks_to_db = value.get_boolean();
    }

    // Parse static_nodes
    auto static_nodes_field = td::get_json_object_field(root, "static_nodes", td::JsonValue::Type::Array, false);
    if (static_nodes_field.is_ok()) {
      auto& nodes_array = static_nodes_field.move_as_ok().get_array();
      for (size_t i = 0; i < nodes_array.size(); i++) {
        auto& node_elem = nodes_array[i];
        if (node_elem.type() == td::JsonValue::Type::Object) {
          auto& node_obj = node_elem.get_object();
          auto address_field = td::get_json_object_field(node_obj, "address", td::JsonValue::Type::String, false);
          auto key_field = td::get_json_object_field(node_obj, "key", td::JsonValue::Type::String, false);

          if (address_field.is_ok() && key_field.is_ok()) {
            std::string address = address_field.ok().get_string().str();
            std::string key = key_field.ok().get_string().str();
            config.static_nodes.emplace_back(address, key);
          }
        }
      }
    }

    // Parse overlay_ids
    auto overlay_ids_field = td::get_json_object_field(root, "overlay_ids", td::JsonValue::Type::Array, false);
    if (overlay_ids_field.is_ok()) {
      auto& overlay_array = overlay_ids_field.move_as_ok().get_array();
      for (size_t i = 0; i < overlay_array.size(); i++) {
        auto& overlay_elem = overlay_array[i];
        if (overlay_elem.type() == td::JsonValue::Type::String) {
          config.overlay_ids.push_back(overlay_elem.get_string().str());
        }
      }
    }

    return config;
  }

  // Загрузка из файла
  static ListenerHeadConfig load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
      LOG(WARNING) << "Cannot open config file: " << filename << ", using default config";
      return ListenerHeadConfig();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return load_from_json(buffer.str());
  }

  // Преобразование в строковое представление
  std::string to_string() const {
    std::string result = "ListenerHeadConfig {\n";
    result += "  http_port: " + std::to_string(http_port) + "\n";
    result += "  log_level: " + std::to_string(log_level) + "\n";
    result += "  max_connections: " + std::to_string(max_connections) + "\n";
    result += "  udp_buffer_size: " + std::to_string(udp_buffer_size) + "\n";
    result += "  max_blocks_to_store: " + std::to_string(max_blocks_to_store) + "\n";
    result += "  save_blocks_to_db: " + std::string(save_blocks_to_db ? "true" : "false") + "\n";

    result += "  static_nodes: [\n";
    for (const auto& node : static_nodes) {
      result += "    { address: \"" + node.first + "\", key: \"" + node.second + "\" },\n";
    }
    result += "  ]\n";

    result += "  overlay_ids: [\n";
    for (const auto& id : overlay_ids) {
      result += "    \"" + id + "\",\n";
    }
    result += "  ]\n";

    result += "}\n";
    return result;
  }
};
} // namespace listener
} // namespace ton