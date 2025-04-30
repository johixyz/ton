#pragma once

#include "common/refcnt.hpp"
#include "ton/ton-types.h"
#include "adnl/adnl-node-id.hpp"
#include "td/utils/Time.h"
#include "td/utils/port/IPAddress.h"

#include <map>
#include <deque>
#include <vector>
#include <mutex>

namespace ton {
namespace listener {

struct BlockReceptionStats {
  BlockIdExt block_id;
  adnl::AdnlNodeIdShort source_node;
  td::Timestamp received_at;
  size_t message_size;
  td::IPAddress source_addr;
  double processing_time{0.0};
};

class BlockReceptionTracker : public td::CntObject {
 public:
  void track_block_received(BlockIdExt block_id, adnl::AdnlNodeIdShort source_node,
                            td::Timestamp received_at, size_t message_size,
                            td::IPAddress source_addr = td::IPAddress(),
                            double processing_time = 0.0) {
    std::lock_guard<std::mutex> guard(mutex_);

    BlockReceptionStats stats;
    stats.block_id = block_id;
    stats.source_node = source_node;
    stats.received_at = received_at;
    stats.message_size = message_size;
    stats.source_addr = source_addr;
    stats.processing_time = processing_time;

    recent_blocks_[block_id] = stats;
    reception_history_.push_back(stats);

    // Обновляем статистику и поддерживаем ограниченный размер истории
    update_stats();

    if (reception_history_.size() > max_history_size_) {
      reception_history_.pop_front();
    }
  }

  std::vector<BlockReceptionStats> get_recent_blocks_stats(int limit = 100) {
    std::lock_guard<std::mutex> guard(mutex_);

    std::vector<BlockReceptionStats> result;
    result.reserve(std::min(reception_history_.size(), static_cast<size_t>(limit)));

    int count = 0;
    for (auto it = reception_history_.rbegin(); it != reception_history_.rend() && count < limit; ++it, ++count) {
      result.push_back(*it);
    }

    return result;
  }

  BlockReceptionStats get_block_stats(BlockIdExt block_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = recent_blocks_.find(block_id);
    if (it != recent_blocks_.end()) {
      return it->second;
    }
    return BlockReceptionStats{};
  }

  double get_average_processing_time() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return avg_processing_time_;
  }

  size_t get_blocks_received_count() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return blocks_received_;
  }

  void clear_old_blocks(td::Timestamp older_than) {
    std::lock_guard<std::mutex> guard(mutex_);

    auto it = reception_history_.begin();
    while (it != reception_history_.end()) {
      if (it->received_at.at() < older_than.at()) {
        recent_blocks_.erase(it->block_id);
        it = reception_history_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  void update_stats() {
    blocks_received_++;

    if (!reception_history_.empty()) {
      double alpha = 0.1; // Коэффициент сглаживания
      auto& last = reception_history_.back();
      avg_processing_time_ = alpha * last.processing_time + (1.0 - alpha) * avg_processing_time_;
    }
  }

  std::map<BlockIdExt, BlockReceptionStats> recent_blocks_;
  std::deque<BlockReceptionStats> reception_history_;
  size_t blocks_received_{0};
  double avg_processing_time_{0.0};

  static constexpr size_t max_history_size_ = 10000;
  mutable std::mutex mutex_;
};

} // namespace listener
} // namespace ton
