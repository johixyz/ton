#pragma once

#include "validator/validator.h"
#include "validator/manager.hpp"
#include "block-reception-tracker.hpp"

#include <atomic>

namespace ton {
namespace listener {

class ListenerHeadManager : public validator::ValidatorManager {
 public:
  void install_callback(std::unique_ptr<validator::Callback> new_callback, td::Promise<td::Unit> promise) override {
    callback_ = std::move(new_callback);
    promise.set_value(td::Unit());
  }

  // Базовые функции управления ключами
  void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    promise.set_value(td::Unit());
  }

  void add_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    promise.set_value(td::Unit());
  }

  void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    promise.set_value(td::Unit());
  }

  void del_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override {
    promise.set_value(td::Unit());
  }

  // Основная функция обработки блоков
  void prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) override {
    auto reception_time = td::Timestamp::now();

    // Регистрируем полученный блок
    track_block_received(broadcast.block_id, broadcast.source_id, reception_time,
                         broadcast.data.size(), broadcast.source_addr);

    // Логируем каждые 100 блоков
    if (++blocks_received_ % 100 == 0) {
      LOG(INFO) << "ListenerHead received " << blocks_received_ << " block broadcasts";
    }

    promise.set_value(td::Unit());
  }

  // Обработка блок-кандидатов
  void new_block_candidate(BlockIdExt block_id, td::BufferSlice data) override {
    auto reception_time = td::Timestamp::now();

    adnl::AdnlNodeIdShort source_id;
    td::IPAddress source_addr;

    track_block_received(block_id, source_id, reception_time, data.size(), source_addr);

    if (++block_candidates_received_ % 100 == 0) {
      LOG(INFO) << "ListenerHead received " << block_candidates_received_ << " block candidates";
    }
  }

  // Обработка шардовых блоков
  void new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override {
    auto reception_time = td::Timestamp::now();

    adnl::AdnlNodeIdShort source_id;
    td::IPAddress source_addr;

    track_block_received(block_id, source_id, reception_time, data.size(), source_addr);

    if (++shard_blocks_received_ % 100 == 0) {
      LOG(INFO) << "ListenerHead received " << shard_blocks_received_ << " shard blocks";
    }
  }

  // Методы для получения статистики
  td::Ref<BlockReceptionTracker> get_block_tracker() const {
    return block_tracker_;
  }

  std::vector<BlockReceptionStats> get_recent_blocks_stats(int limit = 100) {
    return block_tracker_->get_recent_blocks_stats(limit);
  }

  BlockReceptionStats get_block_stats(BlockIdExt block_id) {
    return block_tracker_->get_block_stats(block_id);
  }

  double get_average_processing_time() const {
    return block_tracker_->get_average_processing_time();
  }

  size_t get_blocks_received_count() const {
    return block_tracker_->get_blocks_received_count();
  }

  void start_up() override {
    LOG(INFO) << "ListenerHeadManager started";
    started_ = true;
  }

  // Конструктор
  ListenerHeadManager(td::Ref<validator::ValidatorManagerOptions> opts, std::string db_root,
                      td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                      td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays)
      : opts_(std::move(opts)),
      db_root_(db_root),
      keyring_(keyring),
      adnl_(adnl),
      rldp_(rldp),
      overlays_(overlays) {
    block_tracker_ = td::Ref<BlockReceptionTracker>(true);
  }

  // Перегрузки остальных методов ValidatorManager (добавить заглушки для всех неиспользуемых методов)
  void validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) override {
    promise.set_error(td::Status::Error(ErrorCode::error, "Not implemented in Listener Head"));
  }

  // ... [здесь реализации всех остальных методов интерфейса]

 private:
  void track_block_received(BlockIdExt block_id, adnl::AdnlNodeIdShort source_node,
                            td::Timestamp received_at, size_t message_size,
                            td::IPAddress source_addr = td::IPAddress()) {
    auto processing_time = td::Timestamp::now().at() - received_at.at();
    block_tracker_->track_block_received(block_id, source_node, received_at, message_size,
                                         source_addr, processing_time);
  }

  std::unique_ptr<validator::Callback> callback_;
  td::Ref<validator::ValidatorManagerOptions> opts_;
  std::string db_root_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  std::atomic<bool> started_{false};
  std::atomic<size_t> blocks_received_{0};
  std::atomic<size_t> block_candidates_received_{0};
  std::atomic<size_t> shard_blocks_received_{0};

  td::Ref<BlockReceptionTracker> block_tracker_;
};

// Фабрика для создания ListenerHeadManager
class ListenerHeadManagerFactory {
 public:
  static td::actor::ActorOwn<validator::ValidatorManager> create(
      td::Ref<validator::ValidatorManagerOptions> opts, std::string db_root,
      td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
      td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays) {
    return td::actor::create_actor<ListenerHeadManager>(
        "listener-head", std::move(opts), db_root, keyring, adnl, rldp, overlays);
  }
};

} // namespace listener
} // namespace ton