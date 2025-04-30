#include "auto/tl/ton_api.hpp"
#include "adnl/adnl.h"
#include "adnl/adnl-ext-client.h"
#include "rldp/rldp.h"
#include "dht/dht.h"
#include "validator/manager.h"
#include "overlay/overlays.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/signals.h"

#include "listener-head-options.hpp"
#include "listener-head-manager.hpp"
#include "listener-connection-manager.hpp"
#include "listener-http-server.hpp"
#include "listener-head-config.hpp"

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
  p.set_description("TON Listener Head");

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

  p.add_option('v', "verbosity", "уровень логирования", [&](td::Slice arg) {
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
  if (config_file.empty()) {
    std::cerr << "ОШИБКА: не указан конфигурационный файл (-c)" << std::endl;
    return 2;
  }

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
    ton::listener::ListenerConfig listener_config;
    {
      auto conf_data = td::read_file(config_file).move_as_ok();
      auto conf_json = td::json_decode(conf_data.as_slice()).move_as_ok();
      listener_config = ton::listener::ListenerConfig::from_json(conf_json);
    }

    // Устанавливаем HTTP порт из конфигурации
    http_port = listener_config.http_port;

    // Загрузка глобальной конфигурации
    auto conf_data = td::read_file(global_config).move_as_ok();
    auto config_json = td::json_decode(conf_data.as_slice()).move_as_ok();

    // Получение базового блока из глобальной конфигурации
    auto zero_state_id = ton::BlockIdExt{};

    // Пытаемся извлечь информацию о zero_state из конфигурации
    if (config_json.type() == td::JsonValue::Type::Object) {
      auto &obj = config_json.get_object();
      auto zero_state = td::get_json_object_field(obj, "zero_state", td::JsonValue::Type::Object, td::JsonValue());

      if (zero_state.type() == td::JsonValue::Type::Object) {
        auto &zs_obj = zero_state.get_object();

        auto workchain = td::get_json_object_int_field(zs_obj, "workchain", ton::workchainIdNotYet);
        auto shard = td::get_json_object_long_field(zs_obj, "shard", 0);
        auto seqno = td::get_json_object_int_field(zs_obj, "seqno", 0);

        auto root_hash_str = td::get_json_object_string_field(zs_obj, "root_hash", "");
        auto file_hash_str = td::get_json_object_string_field(zs_obj, "file_hash", "");

        ton::RootHash root_hash;
        ton::FileHash file_hash;

        if (!root_hash_str.empty() && !file_hash_str.empty()) {
          if (root_hash.from_hex(root_hash_str).is_ok() && file_hash.from_hex(file_hash_str).is_ok()) {
            zero_state_id = ton::BlockIdExt{ton::BlockId{workchain, shard, seqno}, root_hash, file_hash};
            LOG(INFO) << "Обнаружен zero_state: " << zero_state_id.to_str();
          }
        }
      }
    }

    if (zero_state_id.is_valid()) {
      LOG(ERROR) << "Не удалось инициализировать zero_state из конфигурации";
      return;
    }

    // Инициализация keyring
    auto keyring = ton::keyring::Keyring::create(db_root + "/keyring");

    // Инициализация ADNL
    auto adnl = ton::adnl::Adnl::create(db_root + "/adnl", keyring.get());

    // Инициализация DHT
    auto dht = ton::dht::Dht::create_global_dht(db_root + "/dht", keyring.get(), adnl.get(), config_json);

    // Инициализация RLDP
    auto rldp = ton::rldp::Rldp::create(adnl.get());

    // Инициализация оверлеев
    auto overlays = ton::overlay::Overlays::create(db_root + "/overlays", keyring.get(), adnl.get(), dht.get());

    // Создание опций для Listener Head
    auto opts = ton::validator::ListenerHeadOptions::create(
        zero_state_id,
        zero_state_id // В Listener Head это значение не важно
    );

    // Создание менеджера Listener Head
    auto validator_manager = ton::listener::ListenerHeadManagerFactory::create(
        opts, db_root + "/listenerstate", keyring.get(), adnl.get(), rldp.get(), overlays.get()
    );

    // Получаем доступ к трекеру блоков
    auto listener_manager =
        td::actor::create_actor<ton::listener::ListenerHeadManager>(
            "listener-head", std::move(opts), db_root + "/listenerstate", keyring.get(), adnl.get(), rldp.get(), overlays.get()
        );

    // Создание HTTP сервера
    auto http_server = td::actor::create_actor<ton::listener::ListenerHttpServer>(
        "http-server", http_port,
        td::actor::create_actor<ton::listener::ListenerHeadManager>(
            "listener-head", std::move(opts), db_root + "/listenerstate", keyring.get(), adnl.get(), rldp.get(), overlays.get()
                )->get_block_tracker()
    );

    // Создание менеджера соединений
    auto connection_manager = td::actor::create_actor<ton::listener::ConnectionManager>(
        "connection-manager", adnl.get(), overlays.get(), dht.get()
    );

    LOG(INFO) << "TON Listener Head успешно запущен на HTTP порту " << http_port;

    // Добавляем мастерчейн оверлей для отслеживания
    if (zero_state_id.is_valid() && zero_state_id.id.workchain == ton::masterchainId) {
      auto overlay_id = ton::overlay::OverlayIdShort{zero_state_id.root_hash};
      LOG(INFO) << "Добавляем мастерчейн оверлей: " << overlay_id.serialize();
      td::actor::send_closure(connection_manager, &ton::listener::ConnectionManager::add_overlay, overlay_id);
    }

    // Устанавливаем максимальное количество соединений
    td::actor::send_closure(connection_manager, &ton::listener::ConnectionManager::set_max_connections,
                            listener_config.max_connections);

    LOG(INFO) << "Инициализация сети завершена";
  });

  // Запуск планировщика
  scheduler.run();

  return 0;
}