// listener-head-manager.cpp
#include "listener-head-manager.hpp"
#include "adnl/utils.hpp"
#include "block-handle.hpp"

namespace ton {
namespace validator {
namespace listener {

td::actor::ActorOwn<ValidatorManagerInterface> ListenerHeadManager::create(
    td::Ref<ValidatorManagerOptions> opts, std::string db_root,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays) {

  return td::actor::create_actor<ListenerHeadManager>(
      "listenerhead", std::move(opts), db_root, keyring, adnl, rldp, overlays);
}

ListenerHeadManager::ListenerHeadManager(
    td::Ref<ValidatorManagerOptions> opts, std::string db_root,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays)
    : opts_(std::move(opts))
    , db_root_(db_root)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp_(rldp)
    , overlays_(overlays) {
}

void ListenerHeadManager::start_up() {
  LOG(WARNING) << "Starting TON Listener Head node (optimized for block reception)";
  alarm_timestamp() = td::Timestamp::in(1.0);

  // Ensure directories exist
  td::mkdir(db_root_).ensure();

  // Initialize connection limits in adnl
  // This would ideally configure higher connection limits

  started_ = true;
}

void ListenerHeadManager::alarm() {
  alarm_timestamp() = td::Timestamp::in(1.0);

  if (next_stats_time_.is_in_past()) {
    print_stats();
    next_stats_time_ = td::Timestamp::in(60.0);
  }

  alarm_timestamp().relax(next_stats_time_);
}

void ListenerHeadManager::print_stats() {
  LOG(WARNING) << "Listener Head Stats: Total blocks received: " << total_blocks_received_
               << ", Currently tracking: " << received_blocks_.size() << " blocks";

  // Could print more detailed stats about the most recent blocks
  if (!received_blocks_.empty()) {
    size_t sample_size = std::min(received_blocks_.size(), size_t(10));
    LOG(WARNING) << "Recent " << sample_size << " blocks:";

    size_t i = 0;
    for (auto it = received_blocks_.rbegin(); it != received_blocks_.rend() && i < sample_size; ++it, ++i) {
      const auto& info = it->second;
      LOG(WARNING) << "  " << info.block_id.to_str()
                   << ", size=" << info.block_size
                   << ", time=" << (double)info.received_at_ms / 1000.0;
    }
  }
}

void ListenerHeadManager::install_callback(std::unique_ptr<Callback> new_callback, td::Promise<td::Unit> promise) {
  callback_ = std::move(new_callback);
  promise.set_value(td::Unit());
}

void ListenerHeadManager::add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  permanent_keys_.insert(key);
  promise.set_value(td::Unit());
}

void ListenerHeadManager::add_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  temp_keys_.insert(key);
  promise.set_value(td::Unit());
}

void ListenerHeadManager::del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  permanent_keys_.erase(key);
  promise.set_value(td::Unit());
}

void ListenerHeadManager::del_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  temp_keys_.erase(key);
  promise.set_value(td::Unit());
}

void ListenerHeadManager::add_ext_server_id(adnl::AdnlNodeIdShort id) {
  // Handle external server ID
  LOG(INFO) << "Added external server ID: " << id;
}

void ListenerHeadManager::add_ext_server_port(td::uint16 port) {
  // Handle external server port
  LOG(INFO) << "Added external server port: " << port;
}

BlockHandle ListenerHeadManager::create_or_get_handle(BlockIdExt id) {
  auto it = handles_.find(id);
  if (it != handles_.end()) {
    auto handle = it->second.lock();
    if (handle) {
      return handle;
    }
  }

  // Create a new empty handle
  auto handle = BlockHandleImpl::create_empty(id);
  handle->set_received(); // Mark as received without validation
  handles_[id] = handle;
  return handle;
}

void ListenerHeadManager::get_block_handle(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) {
  auto handle = create_or_get_handle(id);
  promise.set_value(std::move(handle));
}

void ListenerHeadManager::validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) {
  // Skip validation - just record that we received the block
  LOG(INFO) << "Received block: " << block.id;

  // Record reception information with timing
  record_block_reception(block.id, adnl::AdnlNodeIdShort(), block.data.clone());

  // Create a minimal handle without validation
  auto handle = create_or_get_handle(block.id);

  // Forward to other systems if configured
  forward_block_to_callback(block.id, 0, block.data.clone());

  // Return handle to caller
  promise.set_value(std::move(handle));
}

void ListenerHeadManager::prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) {
  // Skip validation - just record reception
  LOG(INFO) << "Received block broadcast: " << broadcast.block_id;

  // Record reception with timing
  adnl::AdnlNodeIdShort source;
  if (!broadcast.signatures.empty()) {
    source = broadcast.signatures[0].node;
  }
  record_block_reception(broadcast.block_id, source, broadcast.data.clone());

  // Forward to other systems if needed
  forward_block_to_callback(broadcast.block_id, broadcast.catchain_seqno, broadcast.data.clone());

  // Always succeed - no validation
  promise.set_value(td::Unit());
}

void ListenerHeadManager::new_block_candidate(BlockIdExt block_id, td::BufferSlice data) {
  LOG(INFO) << "Received block candidate: " << block_id;

  // Record reception
  record_block_reception(block_id, adnl::AdnlNodeIdShort(), data.clone());

  // Forward to other systems if needed
  forward_block_to_callback(block_id, 0, std::move(data));
}

void ListenerHeadManager::new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  LOG(INFO) << "Received shard block: " << block_id << ", cc_seqno=" << cc_seqno;

  // Record reception
  record_block_reception(block_id, adnl::AdnlNodeIdShort(), data.clone());

  // Forward to other systems if needed
  forward_block_to_callback(block_id, cc_seqno, std::move(data));
}

void ListenerHeadManager::record_block_reception(BlockIdExt block_id, adnl::AdnlNodeIdShort source, td::BufferSlice data) {
  auto now_ms = static_cast<td::uint64>(td::Timestamp::now().at() * 1000);

  // Create reception info
  BlockReceptionInfo info(block_id, now_ms, source, data.size());

  // Store reception information
  received_blocks_[block_id] = info;
  reception_lru_.push(block_id);

  // Maintain size limit with LRU eviction
  if (received_blocks_.size() > max_blocks_to_track_) {
    BlockIdExt to_remove = reception_lru_.front();
    reception_lru_.pop();
    received_blocks_.erase(to_remove);
  }

  total_blocks_received_++;

  // Log meaningful details
  LOG(INFO) << "Block received: " << block_id.to_str()
            << ", size=" << data.size()
            << ", total_received=" << total_blocks_received_;
}

void ListenerHeadManager::forward_block_to_callback(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  if (!callback_) {
    return;
  }

  // Forward to appropriate callback method based on block type
  if (block_id.is_masterchain()) {
    if (cc_seqno == 0) {
      // Regular block - use download_block
      callback_->download_block(block_id, 0, td::Timestamp::in(10.0), [block_id](td::Result<ReceivedBlock> R) {
        if (R.is_error()) {
          LOG(WARNING) << "Failed to forward masterchain block " << block_id << ": " << R.error();
        }
      });
    } else {
      // Block with catchain seqno - use send_shard_block_info
      callback_->send_shard_block_info(block_id, cc_seqno, std::move(data));
    }
  } else {
    // Shard block
    if (cc_seqno == 0) {
      callback_->download_block(block_id, 0, td::Timestamp::in(10.0), [block_id](td::Result<ReceivedBlock> R) {
        if (R.is_error()) {
          LOG(WARNING) << "Failed to forward shard block " << block_id << ": " << R.error();
        }
      });
    } else {
      callback_->send_shard_block_info(block_id, cc_seqno, std::move(data));
    }
  }
}

void ListenerHeadManager::validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id,
                                                       td::BufferSlice proof, td::Promise<td::Unit> promise) {
  LOG(INFO) << "Skipping validation for " << next_block_id;
  promise.set_value(td::Unit());
}

void ListenerHeadManager::validate_block_proof(BlockIdExt block_id, td::BufferSlice proof,
                                               td::Promise<td::Unit> promise) {
  LOG(INFO) << "Skipping proof validation for " << block_id;
  promise.set_value(td::Unit());
}

void ListenerHeadManager::validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof,
                                                    td::Promise<td::Unit> promise) {
  LOG(INFO) << "Skipping proof link validation for " << block_id;
  promise.set_value(td::Unit());
}

void ListenerHeadManager::validate_block_proof_rel(BlockIdExt block_id, BlockIdExt rel_block_id,
                                                   td::BufferSlice proof, td::Promise<td::Unit> promise) {
  LOG(INFO) << "Skipping relative proof validation for " << block_id;
  promise.set_value(td::Unit());
}

// State and block access methods - minimal implementations
void ListenerHeadManager::get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) {
  // We don't maintain state, so return an error
  promise.set_error(td::Status::Error(ErrorCode::notready, "state not maintained in listener head mode"));
}

void ListenerHeadManager::get_top_masterchain_block(td::Promise<BlockIdExt> promise) {
  if (last_masterchain_block_id_.is_valid()) {
    promise.set_value(last_masterchain_block_id_);
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "no masterchain blocks received yet"));
  }
}

void ListenerHeadManager::get_top_masterchain_state_block(
    td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "state not maintained in listener head mode"));
}

void ListenerHeadManager::get_last_liteserver_state_block(
    td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "state not maintained in listener head mode"));
}

void ListenerHeadManager::get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto it = block_data_cache_.find(handle->id());
  if (it != block_data_cache_.end()) {
    promise.set_value(it->second.clone());
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "block data not found"));
  }
}

// Zero state methods
void ListenerHeadManager::check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  promise.set_result(false);
}

void ListenerHeadManager::get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "zero state not maintained"));
}

// Persistent state methods
void ListenerHeadManager::check_persistent_state_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                        td::Promise<bool> promise) {
  promise.set_result(false);
}

void ListenerHeadManager::get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                               td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "persistent state not maintained"));
}

void ListenerHeadManager::get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                     td::int64 offset, td::int64 max_length,
                                                     td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "persistent state not maintained"));
}

void ListenerHeadManager::get_previous_persistent_state_files(
    BlockSeqno cur_mc_seqno, td::Promise<std::vector<std::pair<std::string, ShardIdFull>>> promise) {
  promise.set_value(std::vector<std::pair<std::string, ShardIdFull>>{});
}

// Proof methods
void ListenerHeadManager::get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "proofs not maintained"));
}

void ListenerHeadManager::get_block_proof_link(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "proof links not maintained"));
}

void ListenerHeadManager::get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "key block proofs not maintained"));
}

void ListenerHeadManager::get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "key block proof links not maintained"));
}

// Key block methods
void ListenerHeadManager::get_next_key_blocks(BlockIdExt block_id, td::uint32 cnt,
                                              td::Promise<std::vector<BlockIdExt>> promise) {
  promise.set_value(std::vector<BlockIdExt>{});
}

void ListenerHeadManager::get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "next block relationships not tracked"));
}

void ListenerHeadManager::write_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  // No persistence, just acknowledge
  promise.set_value(td::Unit());
}

// Sync methods
void ListenerHeadManager::sync_complete(td::Promise<td::Unit> promise) {
  // No sync needed, just acknowledge
  promise.set_value(td::Unit());
}

// Message methods
void ListenerHeadManager::new_external_message(td::BufferSlice data, int priority) {
  // Ignore external messages
  LOG(INFO) << "Ignoring external message in listener mode";
}

void ListenerHeadManager::check_external_message(td::BufferSlice data,
                                                 td::Promise<td::Ref<ExtMessage>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "external messages not supported"));
}

void ListenerHeadManager::new_ihr_message(td::BufferSlice data) {
  // Ignore IHR messages
  LOG(INFO) << "Ignoring IHR message in listener mode";
}

void ListenerHeadManager::run_ext_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "external queries not supported"));
}

// Download methods
void ListenerHeadManager::get_download_token(size_t download_size, td::uint32 priority,
                                             td::Timestamp timeout,
                                             td::Promise<std::unique_ptr<ActionToken>> promise) {
  // Allow all downloads
  class SimpleToken : public ActionToken {
  };
  promise.set_value(std::make_unique<SimpleToken>());
}

// DB access methods - simplified since we don't use DB
void ListenerHeadManager::get_block_data_from_db(ConstBlockHandle handle,
                                                 td::Promise<td::Ref<BlockData>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_data_from_db_short(BlockIdExt block_id,
                                                       td::Promise<td::Ref<BlockData>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_candidate_from_db(PublicKey source, BlockIdExt id,
                                                      FileHash collated_data_file_hash,
                                                      td::Promise<BlockCandidate> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_candidate_data_by_block_id_from_db(BlockIdExt id,
                                                                 td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_shard_state_from_db(ConstBlockHandle handle,
                                                  td::Promise<td::Ref<ShardState>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_shard_state_from_db_short(BlockIdExt block_id,
                                                        td::Promise<td::Ref<ShardState>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_proof_from_db(ConstBlockHandle handle,
                                                  td::Promise<td::Ref<Proof>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_proof_from_db_short(BlockIdExt id,
                                                        td::Promise<td::Ref<Proof>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_proof_link_from_db(ConstBlockHandle handle,
                                                       td::Promise<td::Ref<ProofLink>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_proof_link_from_db_short(BlockIdExt id,
                                                             td::Promise<td::Ref<ProofLink>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_by_lt_from_db(AccountIdPrefixFull account, LogicalTime lt,
                                                  td::Promise<ConstBlockHandle> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_by_unix_time_from_db(AccountIdPrefixFull account, UnixTime ts,
                                                         td::Promise<ConstBlockHandle> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

void ListenerHeadManager::get_block_by_seqno_from_db(AccountIdPrefixFull account, BlockSeqno seqno,
                                                     td::Promise<ConstBlockHandle> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "DB not used in listener mode"));
}

// Wait state methods
void ListenerHeadManager::wait_block_state(BlockHandle handle, td::uint32 priority,
                                           td::Timestamp timeout,
                                           td::Promise<td::Ref<ShardState>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "states not maintained in listener mode"));
}

void ListenerHeadManager::wait_block_state_short(BlockIdExt block_id, td::uint32 priority,
                                                 td::Timestamp timeout,
                                                 td::Promise<td::Ref<ShardState>> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "states not maintained in listener mode"));
}

// Archive methods
void ListenerHeadManager::get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                         td::Promise<td::uint64> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "archives not maintained in listener mode"));
}

void ListenerHeadManager::get_archive_slice(td::uint64 archive_id, td::uint64 offset,
                                            td::uint32 limit, td::Promise<td::BufferSlice> promise) {
  promise.set_error(td::Status::Error(ErrorCode::notready, "archives not maintained in listener mode"));
}

// Stats methods
void ListenerHeadManager::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  std::vector<std::pair<std::string, std::string>> stats;

  // Basic stats
  stats.emplace_back("mode", "listener_head");
  stats.emplace_back("total_blocks_received", std::to_string(total_blocks_received_));
  stats.emplace_back("tracked_blocks", std::to_string(received_blocks_.size()));

  if (last_masterchain_block_id_.is_valid()) {
    stats.emplace_back("last_masterchain_block", last_masterchain_block_id_.to_str());
    stats.emplace_back("last_masterchain_seqno", std::to_string(last_masterchain_seqno_));
    auto now = td::Timestamp::now();
    stats.emplace_back("last_masterchain_block_age",
                       std::to_string(now.at() - received_masterchain_block_at_.at()));
  }

  // Get stats from providers
  for (auto& [_, provider] : stats_providers_) {
    auto P = td::PromiseCreator::lambda([&stats, prefix = provider.first](td::Result<std::vector<std::pair<std::string, std::string>>> R) {
      if (R.is_ok()) {
        auto provider_stats = R.move_as_ok();
        for (auto& [key, value] : provider_stats) {
          stats.emplace_back(prefix + key, value);
        }
      }
    });
    provider.second(std::move(P));
  }

  promise.set_value(std::move(stats));
}

void ListenerHeadManager::prepare_actor_stats(td::Promise<std::string> promise) {
  promise.set_value("No actor stats in listener head mode");
}

void ListenerHeadManager::prepare_perf_timer_stats(td::Promise<std::vector<PerfTimerStats>> promise) {
  promise.set_value(std::vector<PerfTimerStats>{});
}

void ListenerHeadManager::add_perf_timer_stat(std::string name, double duration) {
  // Ignore perf timer stats
}

void ListenerHeadManager::get_out_msg_queue_size(BlockIdExt block_id, td::Promise<td::uint64> promise) {
  promise.set_result(0);
}

// Config methods
void ListenerHeadManager::update_options(td::Ref<ValidatorManagerOptions> opts) {
  opts_ = std::move(opts);
}

// Stats provider methods
void ListenerHeadManager::register_stats_provider(
    td::uint64 idx, std::string prefix,
    std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)> callback) {
  stats_providers_[idx] = {std::move(prefix), std::move(callback)};
}

void ListenerHeadManager::unregister_stats_provider(td::uint64 idx) {
  stats_providers_.erase(idx);
}


} // namespace listener
} // namespace validator
} // namespace ton
