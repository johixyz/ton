#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>

namespace ton {
namespace listener {

// Конфигурация ListenerHead
struct ListenerHeadConfig {
  // Общие настройки
  int http_port = 8080;
  int log_level = 3;
  int max_connections = 1000;
  int udp_buffer_size = 10 * 1024 * 1024;

  // Сетевые настройки
  std::vector<std::pair<std::string, std::string>> static_nodes; // пары (адрес, ключ)
  std::vector<std::string> overlay_ids;                          // оверлеи для мониторинга

  // Настройки хранения
  int max_blocks_to_store = 10000;
  bool save_blocks_to_db = false;

  // Простой парсер для загрузки из JSON
  static ListenerHeadConfig load_from_json(const std::string& json_data) {
    ListenerHeadConfig config;

    // Простой парсинг (очень базовый, для реального использования лучше взять нормальную библиотеку)

    // Парсинг HTTP порта
    if (json_data.find("\"http_port\":") != std::string::npos) {
      size_t pos = json_data.find("\"http_port\":");
      pos = json_data.find_first_of("0123456789", pos);
      if (pos != std::string::npos) {
        config.http_port = std::stoi(json_data.substr(pos));
      }
    }

    // Парсинг уровня логирования
    if (json_data.find("\"log_level\":") != std::string::npos) {
      size_t pos = json_data.find("\"log_level\":");
      pos = json_data.find_first_of("0123456789", pos);
      if (pos != std::string::npos) {
        config.log_level = std::stoi(json_data.substr(pos));
      }
    }

    // Парсинг max_connections
    if (json_data.find("\"max_connections\":") != std::string::npos) {
      size_t pos = json_data.find("\"max_connections\":");
      pos = json_data.find_first_of("0123456789", pos);
      if (pos != std::string::npos) {
        config.max_connections = std::stoi(json_data.substr(pos));
      }
    }

    // Парсинг udp_buffer_size
    if (json_data.find("\"udp_buffer_size\":") != std::string::npos) {
      size_t pos = json_data.find("\"udp_buffer_size\":");
      pos = json_data.find_first_of("0123456789", pos);
      if (pos != std::string::npos) {
        config.udp_buffer_size = std::stoi(json_data.substr(pos));
      }
    }

    // Более сложные поля (как static_nodes, overlay_ids) требуют более
    // сложного парсинга, здесь для простоты опущены

    return config;
  }

  // Загрузка из файла
  static ListenerHeadConfig load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
      // В случае ошибки возвращаем конфигурацию по умолчанию
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