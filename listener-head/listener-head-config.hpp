#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <json/json.h>
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"

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

    try {
      auto parser = td::json_decode(json_data);

      if (parser.has_key("http_port")) {
        config.http_port = static_cast<td::uint16>(parser.get_number("http_port"));
      }

      if (parser.has_key("log_level")) {
        config.log_level = static_cast<int>(parser.get_number("log_level"));
      }

      if (parser.has_key("max_connections")) {
        config.max_connections = static_cast<int>(parser.get_number("max_connections"));
      }

      if (parser.has_key("udp_buffer_size")) {
        config.udp_buffer_size = static_cast<int>(parser.get_number("udp_buffer_size"));
      }

      if (parser.has_key("max_blocks_to_store")) {
        config.max_blocks_to_store = static_cast<int>(parser.get_number("max_blocks_to_store"));
      }

      if (parser.has_key("save_blocks_to_db")) {
        config.save_blocks_to_db = parser.get_boolean("save_blocks_to_db");
      }

      // Парсинг static_nodes
      if (parser.has_key("static_nodes") && parser.has_array("static_nodes")) {
        auto nodes_array = parser.get_array("static_nodes");
        for (auto& node_elem : nodes_array) {
          if (node_elem.has_key("address") && node_elem.has_key("key")) {
            std::string address = node_elem.get_string("address");
            std::string key = node_elem.get_string("key");
            config.static_nodes.emplace_back(address, key);
          }
        }
      }

      // Парсинг overlay_ids
      if (parser.has_key("overlay_ids") && parser.has_array("overlay_ids")) {
        auto overlay_array = parser.get_array("overlay_ids");
        for (auto& overlay_elem : overlay_array) {
          config.overlay_ids.push_back(overlay_elem.get_string());
        }
      }

    } catch (const std::exception& e) {
      LOG(ERROR) << "Error parsing JSON config: " << e.what();
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