#pragma once

#include "td/actor/actor.h"
#include "adnl/adnl.h"
#include "overlay/overlays.h"
#include "dht/dht.h"
#include "ton/ton-types.h"
#include "block-reception-tracker.hpp"
#include "td/utils/overloaded.h"
#include "listener-connection-manager.hpp"

namespace ton {
namespace listener {

// Основной класс управления прослушивателем блоков TON
class ListenerHeadManager : public td::actor::Actor {
 public:
  ListenerHeadManager(std::string db_root,
                      td::actor::ActorId<keyring::Keyring> keyring,
                      td::actor::ActorId<adnl::Adnl> adnl,
                      td::actor::ActorId<overlay::Overlays> overlays,
                      td::actor::ActorId<dht::Dht> dht,
                      td::actor::ActorId<ListenerConnectionManager> connection_manager)
      : db_root_(db_root),
      keyring_(keyring),
      adnl_(adnl),
      overlays_(overlays),
      dht_(dht),
      connection_manager_(connection_manager) {
    block_tracker_ = std::make_shared<BlockReceptionTracker>();
    last_block_received_at_ = td::Timestamp::now();
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

  // Добавление оверлея для прослушивания
  void add_overlay_to_listen(overlay::OverlayIdShort overlay_id);

  // Добавление известного валидатора для приоритетного подключения
  void add_known_validator(adnl::AdnlNodeIdShort validator_id, td::IPAddress addr);

  // Установка локального ADNL ID для подключения к оверлеям
  void set_local_id(adnl::AdnlNodeIdShort local_id) {
    local_id_ = local_id;
  }

  // Методы для обработки сообщений оверлея
  void process_overlay_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data);
  void process_broadcast_message(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data);

  // Методы для обработки различных типов broadcast сообщений
  void process_block_broadcast(adnl::AdnlNodeIdShort src, td::Timestamp reception_time, ton_api::overlay_broadcast& msg);
  void process_block_broadcast_fec(adnl::AdnlNodeIdShort src, td::Timestamp reception_time, ton_api::overlay_broadcastFec& msg);
  void process_block_broadcast_fec_short(adnl::AdnlNodeIdShort src, td::Timestamp reception_time, ton_api::overlay_broadcastFecShort& msg);
  void process_block_unicast(adnl::AdnlNodeIdShort src, td::Timestamp reception_time, ton_api::overlay_unicast& msg);

  // Метод для минимальной обработки блока
  void try_process_block(BlockIdExt block_id, td::BufferSlice data, std::string source);

  // Методы жизненного цикла актора
  void start_up() override;
  void alarm() override;
  void tear_down() override {
    LOG(INFO) << "ListenerHeadManager shutting down...";
    stop_listening();
  }

 private:
  // Методы для прослушивания и подключения
  void start_listening();
  void start_listening_overlay(overlay::OverlayIdShort overlay_id);
  void stop_listening();
  void check_connection_status();

  void track_block_received(BlockIdExt block_id, std::string source_id,
                            td::Timestamp received_at, size_t message_size,
                            std::string source_addr = "", double processing_time = 0.0) {
    block_tracker_->track_block_received(block_id, source_id, received_at, message_size,
                                         source_addr, processing_time);
    last_block_received_at_ = td::Timestamp::now();
  }

  // Базовые поля
  std::string db_root_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<dht::Dht> dht_;
  td::actor::ActorId<ListenerConnectionManager> connection_manager_;
  adnl::AdnlNodeIdShort local_id_;

  // Статистика
  std::atomic<size_t> blocks_received_{0};
  std::atomic<size_t> block_candidates_received_{0};
  std::atomic<size_t> shard_blocks_received_{0};
  size_t last_blocks_received_count_{0};

  // Время последнего полученного блока для проверки подключения
  td::Timestamp last_block_received_at_;

  // Трекер блоков
  std::shared_ptr<BlockReceptionTracker> block_tracker_;

  // Мониторимые оверлеи
  std::set<overlay::OverlayIdShort> monitored_overlays_;
};

} // namespace listener
} // namespace ton