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
    // Загрузка глобальной конфигурации
    auto conf_data = td::read_file(global_config).move_as_ok();
    auto conf_json = td::json_decode(conf_data.as_slice()).move_as_ok();

    ton::ton_api::config_global conf;
    ton::ton_api::from_json(conf, conf_json.get_object()).ensure();

    // Инициализация базового блока
    auto zero_state_id = ton::BlockIdExt{
        ton::BlockId{conf.zero_state_->workchain_, conf.zero_state_->shard_, conf.zero_state_->seqno_},
        conf.zero_state_->root_hash_, conf.zero_state_->file_hash_
    };

    // Инициализация keyring
    auto keyring = ton::keyring::Keyring::create(db_root + "/keyring");

    // Инициализация ADNL
    auto adnl = ton::adnl::Adnl::create(db_root + "/adnl", keyring.get());

    // Инициализация DHT
    auto dht_config = ton::dht::DhtGlobalConfig::create(conf.dht_);
    auto dht = ton::dht::Dht::create(db_root + "/dht", dht_config, keyring.get(), adnl.get());

    // Инициализация RLDP
    auto rldp = ton::rldp::Rldp::create(adnl.get());

    // Инициализация RLDP2 (если используется)
    auto rldp2 = ton::rldp2::Rldp::create(adnl.get());

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
        std::static_pointer_cast<ton::listener::ListenerHeadManager>(
            td::actor::Actor::actor_dynamic_cast(validator_manager)
        );

    // Создание HTTP сервера
    auto http_server = td::actor::create_actor<ton::listener::ListenerHttpServer>(
        "http-server", http_port, listener_manager->get_block_tracker()
    );

    // Создание менеджера соединений
    auto connection_manager = td::actor::create_actor<ton::listener::ConnectionManager>(
        "connection-manager", adnl.get(), overlays.get(), dht.get()
    );

    LOG(INFO) << "TON Listener Head успешно запущен на HTTP порту " << http_port;

    // Извлечение информации о начальных узлах из global.config.json
    LOG(INFO) << "Извлечение информации о сети из global.config.json...";

    // Создаем объект для хранения сетевых настроек
    ton::listener::ListenerConfig listener_config;
    {
      auto conf_data = td::read_file(config_file).move_as_ok();
      auto conf_json = td::json_decode(conf_data.as_slice()).move_as_ok();
      listener_config = ton::listener::ListenerConfig::from_json(conf_json);
    }

    // Устанавливаем HTTP порт из конфигурации
    http_port = listener_config.http_port;

    // Получаем информацию о dht-узлах из конфигурации
    for (const auto& dht_node : conf.dht_->static_nodes_.value()) {
      if (dht_node->version_ >= 1 && dht_node->signature_.size() > 0) {
        auto node_id = ton::adnl::AdnlNodeIdShort{dht_node->id_};
        auto addr_list = dht_node->addr_list_;

        if (addr_list && !addr_list->addrs_.empty()) {
          for (const auto& addr : addr_list->addrs_) {
            if (addr->get_id() == ton::ton_api::adnl_address_udp::ID) {
              const auto& udp_addr = static_cast<const ton::ton_api::adnl_address_udp&>(*addr);
              td::IPAddress ip_addr;
              ip_addr.init_ipv4_port(udp_addr.ip_, udp_addr.port_).ensure();

              LOG(INFO) << "Добавляем DHT узел: " << node_id.to_hex() << " на "
                        << ip_addr.get_ip_str() << ":" << ip_addr.get_port();

              td::actor::send_closure(connection_manager, &ton::listener::ConnectionManager::add_peer,
                                      node_id, ip_addr, false);
            }
          }
        }
      }
    }

    // Получаем информацию о валидаторах из конфигурации (если доступна)
    if (conf.validator_ && conf.validator_->init_block_validators_) {
      for (const auto& val_desc : conf.validator_->init_block_validators_->list_) {
        if (val_desc->public_key_hash_.length() == 32) {
          auto key_hash = td::Bits256(val_desc->public_key_hash_);
          LOG(INFO) << "Найден валидатор с ключом: " << key_hash.to_hex();

          // Здесь мы не можем напрямую получить ADNL адрес валидатора,
          // но можем добавить его как приоритетный узел для поиска
          td::actor::send_closure(overlays, &ton::overlay::Overlays::add_priority_node, key_hash);
        }
      }
    }

    // Информация об оверлеях - мастерчейн всегда используется
    ton::overlay::OverlayIdShort masterchain_overlay;

    if (opts_->get_zero_block_id().workchain == ton::masterchainId) {
      auto root_hash = opts_->get_zero_block_id().root_hash;
      masterchain_overlay = ton::overlay::OverlayIdShort{root_hash};

      LOG(INFO) << "Добавляем мастерчейн оверлей: " << root_hash.to_hex();
      td::actor::send_closure(connection_manager, &ton::listener::ConnectionManager::add_overlay,
                              masterchain_overlay);
    }

    // Установка максимального количества соединений
    td::actor::send_closure(connection_manager, &ton::listener::ConnectionManager::set_max_connections,
                            listener_config.max_connections);

    // Настройка UDP буферов если указано в конфигурации
    if (listener_config.udp_buffer_size > 0) {
      td::actor::send_closure(adnl, &ton::adnl::Adnl::set_udp_buffer_size,
                              static_cast<size_t>(listener_config.udp_buffer_size));
    }

    LOG(INFO) << "Инициализация сети завершена";

  });

  // Запуск планировщика
  scheduler.run();

  return 0;
}
