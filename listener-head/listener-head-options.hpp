#pragma once

#include "validator/validator-options.hpp"

namespace ton {
namespace validator {

class ListenerHeadOptions : public ValidatorManagerOptionsImpl {
 public:
  ListenerHeadOptions(BlockIdExt zero_block_id, BlockIdExt init_block_id,
                      std::function<bool(ShardIdFull)> check_shard, bool allow_blockchain_init,
                      double sync_blocks_before, double block_ttl, double state_ttl, double max_mempool_num,
                      double archive_ttl, double key_proof_ttl, bool initial_sync_disabled)
      : ValidatorManagerOptionsImpl(zero_block_id, init_block_id, std::move(check_shard), allow_blockchain_init,
                                    sync_blocks_before, block_ttl, state_ttl, max_mempool_num,
                                    archive_ttl, key_proof_ttl, initial_sync_disabled) {
  }

  bool need_monitor(ShardIdFull shard, const td::Ref<MasterchainState>& state) const override {
    return true; // Мониторим все шарды
  }

  bool allow_blockchain_init() const override {
    return false; // Не инициализируем блокчейн
  }

  double sync_blocks_before() const override {
    return 0; // Не синхронизируем блоки
  }

  double state_ttl() const override {
    return 0; // Не храним состояние
  }

  bool initial_sync_disabled() const override {
    return true; // Отключаем начальную синхронизацию
  }

  static td::Ref<ListenerHeadOptions> create(BlockIdExt zero_block_id, BlockIdExt init_block_id) {
    auto check_shard = [](ShardIdFull shard) { return true; };

    return td::make_ref<ListenerHeadOptions>(
        zero_block_id, init_block_id, std::move(check_shard),
        false, 0.0, 300.0, 0.0, 0.0, 0.0, 0.0, true
    );
  }

  ListenerHeadOptions *make_copy() const override {
    return new ListenerHeadOptions(*this);
  }
};

} // namespace validator
} // namespace ton
