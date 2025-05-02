#include "adnl/adnl.h"
#include "adnl/adnl-ext-client.h"
#include "rldp/rldp.h"
#include "dht/dht.h"
#include "dht/dht.hpp"
#include "overlay/overlays.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/signals.h"
#include "td/utils/JsonBuilder.h"
#include "keys/encryptor.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include <unistd.h>

#include "listener-head-manager.hpp"
#include "listener-connection-manager.hpp"
#include "listener-http-server.hpp"
#include "listener-head-config.hpp"
#include "global-config-parser.cpp"

#include <iostream>
#include <string>
#include <memory>

int main(int argc, char *argv[]) {
  // Настройка логирования
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  std::string config_file;
  std::string db_root;
  std::string global_config;
  bool daemon_mode = false;
  td::uint16 http_port = 8080;

  // Парсинг аргументов командной строки
  td::OptionsParser p;
  p.set_description("TON Listener Head - инструмент для мониторинга блоков TON");

  p.add_option('c', "config", "конфигурационный файл", [&](td::Slice arg) {
    config_file = arg.str();
    return td::Status::OK();
  });

  p.add_option('D', "db", "корневая директория базы данных", [&](td::Slice arg) {
    db_root = arg.str();
    return td::Status::OK();
  });

  p.add_option('G', "global-config", "глобальный конфигурационный файл", [&](td::Slice arg) {
    global_config = arg.str();
    return td::Status::OK();
  });

  p.add_option('d', "daemonize", "запуск в режиме демона", [&]() {
    daemon_mode = true;
    return td::Status::OK();
  });

  p.add_option('p', "http-port", "порт для HTTP API", [&](td::Slice arg) {
    http_port = td::to_integer<td::uint16>(arg);
    return td::Status::OK();
  });

  p.add_option('v', "verbosity", "уровень логирования (0-9)", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
    return td::Status::OK();
  });

  p.add_option('h', "help", "вывод справки", [&]() {
    char buffer[10240];
    td::StringBuilder sb(td::MutableSlice{buffer, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(0);
    return td::Status::OK();
  });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << "ОШИБКА: " << S.move_as_error().message().str() << std::endl;
    return 1;
  }

  // Проверка обязательных параметров
  if (db_root.empty()) {
    std::cerr << "ОШИБКА: не указана директория базы данных (-D)" << std::endl;
    return 2;
  }

  if (global_config.empty()) {
    std::cerr << "ОШИБКА: не указан глобальный конфигурационный файл (-G)" << std::endl;
    return 2;
  }

  // Запуск в режиме демона если нужно
  if (daemon_mode) {
#if TD_DARWIN || TD_LINUX
    std::cout << "Запуск в режиме демона" << std::endl;
    // Отсоединяемся от терминала
    if (daemon(1, 0) < 0) {
      std::cerr << "ОШИБКА: не удалось запустить в режиме демона" << std::endl;
      return 1;
    }
#else
    std::cerr << "ОШИБКА: режим демона не поддерживается на данной платформе" << std::endl;
    return 1;
#endif
  }

  // Установка обработчиков сигналов
  td::set_default_failure_signal_handler().ensure();

  // Инициализация планировщика
  td::actor::Scheduler scheduler({7});
  scheduler.run_in_context([&] {
    // Загрузка конфигурации listener-head
    ton::listener::ListenerHeadConfig config;
    if (!config_file.empty()) {
      try {
        LOG(INFO) << "Загрузка конфигурации из " << config_file;
        config = ton::listener::ListenerHeadConfig::load_from_file(config_file);
        http_port = config.http_port;
        SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + config.log_level);
        LOG(INFO) << "Конфигурация загружена: " << config.to_string();
      } catch (const std::exception& e) {
        LOG(ERROR) << "Ошибка при загрузке конфигурации: " << e.what();
      }
    }

    LOG(INFO) << "Инициализация компонентов TON...";

    // Загрузка и парсинг глобальной конфигурации
    std::string global_config_data;
    try {
      auto res = td::read_file(global_config);
      if (res.is_error()) {
        LOG(ERROR) << "Ошибка при чтении глобального конфига: " << res.move_as_error();
        return;
      }
      global_config_data = res.move_as_ok().as_slice().str();
    } catch (const std::exception& e) {
      LOG(ERROR) << "Ошибка при загрузке глобального конфига: " << e.what();
      return;
    }

    // Создаем базовые компоненты TON
    auto keyring = ton::keyring::Keyring::create(db_root + "/keyring");

    // Генерируем локальный ключ для DHT
    auto private_key = ton::PrivateKey(ton::privkeys::Ed25519::random());
    auto id = ton::adnl::AdnlNodeIdShort{private_key.compute_short_id()};

    // Добавляем ключ в keyring
    auto promise = td::PromiseCreator::lambda([](td::Result<td::Unit> result) {
      if (result.is_error()) {
        LOG(ERROR) << "Error adding key to keyring: " << result.error();
      }
    });
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, private_key, false, std::move(promise));

    // Создаем ADNL
    auto adnl = ton::adnl::Adnl::create(db_root + "/adnl", keyring.get());

    // Получаем список статических узлов из глобальной конфигурации
    auto static_nodes_r = ton::listener::GlobalConfigParser::parse_static_nodes(global_config_data);
    if (static_nodes_r.is_error()) {
      LOG(ERROR) << "Failed to parse static nodes: " << static_nodes_r.error();
      return;
    }
    auto static_nodes = static_nodes_r.move_as_ok();
    LOG(INFO) << "Parsed " << static_nodes.size() << " static nodes from global config";

    // Извлекаем параметры DHT из глобальной конфигурации
    auto json_parse_result = td::json_decode(global_config_data);
    if (json_parse_result.is_error()) {
      LOG(ERROR) << "Failed to parse global config JSON: " << json_parse_result.error();
      return;
    }
    auto config_json = json_parse_result.move_as_ok();

    // Получаем значения k и a из JSON
    td::int32 k_value = 6;
    td::int32 a_value = 3;

    // Проверяем, есть ли раздел dht в глобальной конфигурации
    if (config_json.has_key("dht")) {
      auto dht_config = config_json.get_object_field("dht");
      if (dht_config.has_key("k")) {
        k_value = dht_config.get_number_field("k").value();
      }
      if (dht_config.has_key("a")) {
        a_value = dht_config.get_number_field("a").value();
      }
    }

    // Создаем конфигурацию DHT
    auto dht_config_tl = ton::create_tl_object<ton::ton_api::dht_config_global>(
        ton::create_tl_object<ton::ton_api::dht_nodes>(std::vector<ton::tl_object_ptr<ton::ton_api::dht_node>>()),
        k_value,
        a_value
    );

    // Создаем конфигурацию
    auto dht_config_res = ton::dht::Dht::create_global_config(std::move(dht_config_tl));
    if (dht_config_res.is_error()) {
      LOG(ERROR) << "Failed to create DHT config: " << dht_config_res.error().message();
      return;
    }
    auto dht_config = dht_config_res.move_as_ok();

    // Создаем DHT
    auto dht_res = ton::dht::Dht::create(id, db_root + "/dht", dht_config, keyring.get(), adnl.get());
    if (dht_res.is_error()) {
      LOG(ERROR) << "Failed to create DHT: " << dht_res.error().message();
      return;
    }
    auto dht = dht_res.move_as_ok();

    // Создаем RLDP и overlay
    auto rldp = ton::rldp::Rldp::create(adnl.get());
    auto overlays = ton::overlay::Overlays::create(db_root + "/overlays", keyring.get(), adnl.get(), dht.get());

    LOG(INFO) << "Создание компонентов ListenerHead...";

    // Создаем компоненты ListenerHead
    auto connection_manager = td::actor::create_actor<ton::listener::ListenerConnectionManager>(
        "connection-manager", adnl.get(), overlays.get(), dht.get()
    );

    // Устанавливаем ADNL ID для ConnectionManager
    td::actor::send_closure(connection_manager, &ton::listener::ListenerConnectionManager::set_local_id, id);

    // Устанавливаем максимальное количество соединений из конфигурации
    td::actor::send_closure(connection_manager, &ton::listener::ListenerConnectionManager::set_max_connections,
                            config.max_connections);

    // Создаем ListenerHeadManager
    auto listener_manager = td::actor::create_actor<ton::listener::ListenerHeadManager>(
        "listener-head", db_root, keyring.get(), adnl.get(), overlays.get(), dht.get(), connection_manager.get()
    );

    // Устанавливаем локальный ADNL ID для ListenerHeadManager
    td::actor::send_closure(listener_manager, &ton::listener::ListenerHeadManager::set_local_id, id);

    // Добавляем известных валидаторов и статические узлы
    for (const auto& node : static_nodes) {
      LOG(INFO) << "Adding static node: " << node.to_string();
      td::actor::send_closure(connection_manager, &ton::listener::ListenerConnectionManager::add_peer,
                              node.id_short, node.addr, true);
    }

    // Получаем оверлеи для мониторинга
    auto default_overlays = ton::listener::GlobalConfigParser::extract_default_overlay_ids();
    LOG(INFO) << "Extracted " << default_overlays.size() << " default overlay IDs";

    // Добавляем оверлеи для мониторинга из конфигурации и дефолтные
    for (const auto& overlay_id_str : config.overlay_ids) {
      try {
        // Преобразуем строку hex в OverlayIdShort
        td::Bits256 bits = td::Bits256::zero();
        auto r = td::hex_decode(overlay_id_str);
        if (r.is_ok() && r.ok().size() == bits.as_slice().size() &&
            r.ok().copy_to(bits.as_slice()).is_ok()) {
          auto overlay_id = ton::overlay::OverlayIdShort{bits};
          td::actor::send_closure(listener_manager, &ton::listener::ListenerHeadManager::add_overlay_to_listen, overlay_id);
          td::actor::send_closure(connection_manager, &ton::listener::ListenerConnectionManager::add_overlay, overlay_id);
        } else {
          LOG(ERROR) << "Invalid overlay ID in config: " << overlay_id_str;
        }
      } catch (const std::exception& e) {
        LOG(ERROR) << "Error parsing overlay ID: " << e.what();
      }
    }

    // Добавляем дефолтные оверлеи если в конфигурации ничего не указано
    if (config.overlay_ids.empty()) {
      for (const auto& overlay_id_full : default_overlays) {
        auto overlay_id = overlay_id_full.compute_short_id();
        LOG(INFO) << "Adding default overlay: " << overlay_id.bits256_value();
        td::actor::send_closure(listener_manager, &ton::listener::ListenerHeadManager::add_overlay_to_listen, overlay_id);
        td::actor::send_closure(connection_manager, &ton::listener::ListenerConnectionManager::add_overlay, overlay_id);
      }
    }

    // Создаем HTTP сервер для API
    auto http_server = td::actor::create_actor<ton::listener::ListenerHttpServer>(
        "http-server", http_port, listener_manager.get_actor_unsafe().get_block_tracker()
    );

    LOG(INFO) << "TON Listener Head успешно запущен на HTTP порту " << http_port;
    LOG(INFO) << "Веб-интерфейс доступен по адресу http://localhost:" << http_port;
  });

  // Запуск планировщика
  scheduler.run();

  return 0;
}