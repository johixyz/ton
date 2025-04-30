#pragma once

#include "td/actor/actor.h"
#include "adnl/adnl.h"
#include "overlay/overlays.h"
#include "dht/dht.h"
#include "ton/ton-types.h"
#include "block-reception-tracker.hpp"
#include "td/utils/overloaded.h"

namespace ton {
namespace listener {

// Основной класс управления прослушивателем блоков TON
class ListenerHeadManager : public td::actor::Actor {
 public:
  ListenerHeadManager(std::string db_root,
                      td::actor::ActorId<keyring::Keyring> keyring,
                      td::actor::ActorId<adnl::Adnl> adnl,
                      td::actor::ActorId<overlay::Overlays> overlays,
                      td::actor::ActorId<dht::Dht> dht)
      : db_root_(db_root),
      keyring_(keyring),
      adnl_(adnl),
      overlays_(overlays),
      dht_(dht) {
    block_tracker_ = std::make_shared<BlockReceptionTracker>();
  }

  // Обработка нового блока из сети
  void process_block(BlockIdExt block_id, td::BufferSlice data, std::string source_id = "") {
    auto reception_time = td::Timestamp::now();

    // Логика обработки нового блока
    LOG(INFO) << "Received new block: " << block_id.to_str() << " from " << source_id;

    // Записываем статистику приема
    track_block_received(block_id, source_id, reception_time, data.size());

    // Логируем каждые N блоков для наблюдения
    if (++blocks_received_ % 100 == 0) {
      LOG(INFO) << "ListenerHeadManager received " << blocks_received_
                << " blocks, total " << block_tracker_->get_blocks_received_count();
    }

    // Здесь можно добавить дополнительную логику анализа блока
    // например: анализ транзакций, сохранение в БД и т.д.
  }

  // Обработка блока-кандидата
  void process_block_candidate(BlockIdExt block_id, td::BufferSlice data, std::string source_id = "") {
    auto reception_time = td::Timestamp::now();

    LOG(INFO) << "Received block candidate: " << block_id.to_str();

    track_block_received(block_id, source_id, reception_time, data.size(), "", 0.0);

    if (++block_candidates_received_ % 100 == 0) {
      LOG(INFO) << "ListenerHeadManager received " << block_candidates_received_ << " block candidates";
    }
  }

  // Обработка шардового блока
  void process_shard_block(BlockIdExt block_id, td::BufferSlice data, std::string source_id = "") {
    auto reception_time = td::Timestamp::now();

    LOG(INFO) << "Received shard block: " << block_id.to_str();

    track_block_received(block_id, source_id, reception_time, data.size(), "", 0.0);

    if (++shard_blocks_received_ % 100 == 0) {
      LOG(INFO) << "ListenerHeadManager received " << shard_blocks_received_ << " shard blocks";
    }
  }

  // Методы доступа к статистике
  std::shared_ptr<BlockReceptionTracker> get_block_tracker() const {
    return block_tracker_;
  }

  std::vector<BlockReceptionStats> get_recent_blocks_stats(int limit = 100) const {
    return block_tracker_->get_recent_blocks_stats(limit);
  }

  BlockReceptionStats get_block_stats(const std::string& block_id_str) const {
    return block_tracker_->get_block_stats(block_id_str);
  }

  double get_average_processing_time() const {
    return block_tracker_->get_average_processing_time();
  }

  size_t get_blocks_received_count() const {
    return block_tracker_->get_blocks_received_count();
  }

  size_t get_total_bytes() const {
    return block_tracker_->get_total_bytes_received();
  }

  std::string get_full_stats_json() const {
    return block_tracker_->get_full_stats_json();
  }

  // Методы жизненного цикла актора
  void start_up() override {
    LOG(INFO) << "ListenerHeadManager starting up...";

    // Устанавливаем периодический таймер для действий
    alarm_timestamp() = td::Timestamp::in(1.0);

    start_listening();

    LOG(INFO) << "ListenerHeadManager started successfully";
  }

  void alarm() override {
    // Периодические действия
    check_connection_status();

    // Переустанавливаем таймер
    alarm_timestamp() = td::Timestamp::in(60.0); // каждую минуту
  }

  void tear_down() override {
    LOG(INFO) << "ListenerHeadManager shutting down...";
    stop_listening();
  }

  // Добавление оверлея для прослушивания
  void add_overlay_to_listen(overlay::OverlayIdShort overlay_id) {
    LOG(INFO) << "Adding overlay to listen: " << overlay_id.bits256_value();

    // Проверяем, не добавлен ли уже этот оверлей
    if (monitored_overlays_.find(overlay_id) != monitored_overlays_.end()) {
      LOG(INFO) << "Overlay already being monitored";
      return;
    }

    monitored_overlays_.insert(overlay_id);
    start_listening_overlay(overlay_id);
  }

  // Метод для ручного подключения к узлу
  void connect_to_node(adnl::AdnlNodeIdShort node_id, td::IPAddress addr, bool is_validator = false) {
    LOG(INFO) << "Manually connecting to node: " << node_id.bits256_value() << " at " << addr.get_ip_str();

    // Создаем полный идентификатор узла
    // Это упрощенный пример - в реальном коде нужно получить полный ключ
    auto pubkey = ton::PublicKey(ton::pubkeys::Ed25519{node_id.bits256_value()});
    auto full_id = ton::adnl::AdnlNodeIdFull{pubkey};

    // Создаем список адресов
    adnl::AdnlAddressList addr_list;
    addr_list.add_udp_address(addr);

    // Добавляем пир в ADNL
    td::actor::send_closure(adnl_, &adnl::Adnl::add_peer, node_id, full_id, addr_list);
  }

 private:
  // Методы для прослушивания и подключения
  void start_listening() {
    LOG(INFO) << "Starting to listen for blocks...";

    // Здесь должна быть логика подписки на получение блоков
    // через оверлеи, ADNL и другие механизмы TON

    // Временный код для тестирования - сгенерируем тестовый блок
    if (monitored_overlays_.empty()) {
      LOG(WARNING) << "No overlays to monitor yet, listening functionality limited";
    }
  }

  void start_listening_overlay(overlay::OverlayIdShort overlay_id) {
    LOG(INFO) << "Starting to listen for overlay: " << overlay_id.bits256_value();

    // Здесь должен быть код для подписки на сообщения от оверлея
    // Реализация будет зависеть от конкретного API TON
  }

  void stop_listening() {
    // Отписываемся от всех оверлеев
    for (const auto& overlay_id : monitored_overlays_) {
      LOG(INFO) << "Stopping listening for overlay: " << overlay_id.bits256_value();
      // td::actor::send_closure(overlays_, &overlay::Overlays::remove_overlay, overlay_id, ...)
    }
  }

  void check_connection_status() {
    // Проверка статуса соединений и восстановление при необходимости
    LOG(DEBUG) << "Checking connection status...";

    // Можно проверить число полученных блоков за последнее время
    // и предпринять меры если блоки не поступают
  }

  void track_block_received(BlockIdExt block_id, std::string source_id,
                            td::Timestamp received_at, size_t message_size,
                            std::string source_addr = "", double processing_time = 0.0) {
    block_tracker_->track_block_received(block_id, source_id, received_at, message_size,
                                         source_addr, processing_time);
  }

  // Базовые поля
  std::string db_root_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<dht::Dht> dht_;

  // Статистика
  std::atomic<size_t> blocks_received_{0};
  std::atomic<size_t> block_candidates_received_{0};
  std::atomic<size_t> shard_blocks_received_{0};

  // Трекер блоков
  std::shared_ptr<BlockReceptionTracker> block_tracker_;

  // Мониторимые оверлеи
  std::set<overlay::OverlayIdShort> monitored_overlays_;
};

} // namespace listener
} // namespace ton