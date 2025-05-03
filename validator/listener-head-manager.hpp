// listener-head-manager.hpp
#pragma once

#include "validator/interfaces/validator-manager.h"
#include "interfaces/block-handle.h"
#include "common/refcnt.hpp"
#include "td/actor/actor.h"
#include "adnl/adnl.h"
#include "overlay/overlay.h"
#include "dht/dht.h"
#include "rldp/rldp.h"
#include "td/utils/Time.h"
#include "td/utils/port/path.h"
#include "td/utils/overloaded.h"

#include <map>
#include <queue>
#include <set>

namespace ton {
namespace validator {
namespace listener {

// Block reception information structure
struct BlockReceptionInfo {
  BlockIdExt block_id;
  td::uint64 received_at_ms;  // High precision timestamp
  adnl::AdnlNodeIdShort source_node;
  std::string source_ip;
  td::uint32 propagation_hop_count = 0;  // Estimated hop count if available
  double processing_time_ms = 0;
  td::uint32 block_size = 0;
  td::uint32 validator_set_hash = 0;

  BlockReceptionInfo() = default;
  BlockReceptionInfo(BlockIdExt id, td::uint64 time, adnl::AdnlNodeIdShort source, td::uint32 size)
      : block_id(id), received_at_ms(time), source_node(source), block_size(size) {}
};

class ListenerHeadManager : public ValidatorManagerInterface {
 public:
  // Constructor
  ListenerHeadManager(
      td::Ref<ValidatorManagerOptions> opts, std::string db_root,
      td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
      td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays);

  // Core implementation methods
  void install_callback(std::unique_ptr<Callback> new_callback, td::Promise<td::Unit> promise) override;
  void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;
  void add_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;
  void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;
  void del_temp_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;

  // Block reception methods - these are the key ones we'll implement
  void validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) override;
  void prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) override;
  void new_block_candidate(BlockIdExt block_id, td::BufferSlice data) override;
  void new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override;

  // Network methods
  void add_ext_server_id(adnl::AdnlNodeIdShort id) override;
  void add_ext_server_port(td::uint16 port) override;

  // Startup method
  void start_up() override;
  void alarm() override;

  // Block handle method
  void get_block_handle(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) override;

  // Factory method
  static td::actor::ActorOwn<ValidatorManagerInterface> create(
      td::Ref<ValidatorManagerOptions> opts, std::string db_root,
      td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
      td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays);

  // Validation methods - all simplified to avoid actual validation
  void validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id, td::BufferSlice proof,
                                    td::Promise<td::Unit> promise) override;
  void validate_block_proof(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof, td::Promise<td::Unit> promise) override;
  void validate_block_proof_rel(BlockIdExt block_id, BlockIdExt rel_block_id, td::BufferSlice proof,
                                td::Promise<td::Unit> promise) override;

  // State and block access methods
  void get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) override;
  void get_top_masterchain_block(td::Promise<BlockIdExt> promise) override;
  void get_top_masterchain_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;
  void get_last_liteserver_state_block(td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override;
  void get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) override;
  void get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void check_persistent_state_exists(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<bool> promise) override;
  void get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<td::BufferSlice> promise) override;
  void get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                  td::int64 max_length, td::Promise<td::BufferSlice> promise) override;
  void get_previous_persistent_state_files(BlockSeqno cur_mc_seqno,
                                           td::Promise<std::vector<std::pair<std::string, ShardIdFull>>> promise) override;
  void get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void get_block_proof_link(BlockHandle handle, td::Promise<td::BufferSlice> promise) override;
  void get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) override;
  void get_next_key_blocks(BlockIdExt block_id, td::uint32 cnt, td::Promise<std::vector<BlockIdExt>> promise) override;
  void get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) override;
  void write_handle(BlockHandle handle, td::Promise<td::Unit> promise) override;

  // Sync methods
  void sync_complete(td::Promise<td::Unit> promise) override;

  // Message methods
  void new_external_message(td::BufferSlice data, int priority) override;
  void check_external_message(td::BufferSlice data, td::Promise<td::Ref<ExtMessage>> promise) override;
  void new_ihr_message(td::BufferSlice data) override;
  void run_ext_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;

  // Download & access methods
  void get_download_token(size_t download_size, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<std::unique_ptr<ActionToken>> promise) override;
  void get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) override;
  void get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) override;
  void get_block_candidate_from_db(PublicKey source, BlockIdExt id, FileHash collated_data_file_hash,
                                   td::Promise<BlockCandidate> promise) override;
  void get_candidate_data_by_block_id_from_db(BlockIdExt id, td::Promise<td::BufferSlice> promise) override;
  void get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) override;
  void get_shard_state_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) override;
  void get_block_proof_from_db(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) override;
  void get_block_proof_from_db_short(BlockIdExt id, td::Promise<td::Ref<Proof>> promise) override;
  void get_block_proof_link_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ProofLink>> promise) override;
  void get_block_proof_link_from_db_short(BlockIdExt id, td::Promise<td::Ref<ProofLink>> promise) override;
  void get_block_by_lt_from_db(AccountIdPrefixFull account, LogicalTime lt, td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_unix_time_from_db(AccountIdPrefixFull account, UnixTime ts, td::Promise<ConstBlockHandle> promise) override;
  void get_block_by_seqno_from_db(AccountIdPrefixFull account, BlockSeqno seqno, td::Promise<ConstBlockHandle> promise) override;
  void wait_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                        td::Promise<td::Ref<ShardState>> promise) override;
  void wait_block_state_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::Ref<ShardState>> promise) override;

  // Archive methods
  void get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, td::Promise<td::uint64> promise) override;
  void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                         td::Promise<td::BufferSlice> promise) override;

  // Stats methods
  void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) override;
  void prepare_actor_stats(td::Promise<std::string> promise) override;
  void prepare_perf_timer_stats(td::Promise<std::vector<PerfTimerStats>> promise) override;
  void add_perf_timer_stat(std::string name, double duration) override;
  void get_out_msg_queue_size(BlockIdExt block_id, td::Promise<td::uint64> promise) override;

  // Config methods
  void update_options(td::Ref<ValidatorManagerOptions> opts) override;

  // Additional methods for Listener Head
  void register_stats_provider(td::uint64 idx, std::string prefix,
                               std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)> callback) override;
  void unregister_stats_provider(td::uint64 idx) override;


 private:
  // Core data
  std::unique_ptr<Callback> callback_;
  td::Ref<ValidatorManagerOptions> opts_;
  std::string db_root_;

  // Actor IDs
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  // Connection-related state
  std::set<PublicKeyHash> permanent_keys_;
  std::set<PublicKeyHash> temp_keys_;
  bool started_ = false;

  // Block handles cache
  std::map<BlockIdExt, std::weak_ptr<BlockHandleInterface>> handles_;

  // Block reception tracking
  std::map<BlockIdExt, BlockReceptionInfo> received_blocks_;
  std::queue<BlockIdExt> reception_lru_;
  size_t max_blocks_to_track_ = 10000;
  td::int64 total_blocks_received_ = 0;

  // Stats
  td::Timestamp next_stats_time_ = td::Timestamp::in(60.0);

  BlockIdExt last_masterchain_block_id_; // Track the latest known masterchain block
  td::uint32 last_masterchain_seqno_ = 0;
  td::Timestamp received_masterchain_block_at_ = td::Timestamp::now();

  // Records of block data for quick access
  std::map<BlockIdExt, td::BufferSlice> block_data_cache_;
  size_t max_block_data_cache_size_ = 100; // Limit memory usage
  std::queue<BlockIdExt> block_data_lru_;

  // Stats providers
  std::map<td::uint64, std::pair<std::string,
                                 std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)>>> stats_providers_;

  // Methods
  void record_block_reception(BlockIdExt block_id, adnl::AdnlNodeIdShort source, td::BufferSlice data);
  void forward_block_to_callback(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data);
  void print_stats();
  BlockHandle create_or_get_handle(BlockIdExt id);
};

} // namespace listener
} // namespace validator
} // namespace ton