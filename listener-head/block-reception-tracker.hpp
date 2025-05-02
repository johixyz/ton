#pragma once

#include "td/utils/common.h"
#include "td/utils/Time.h"
#include "td/utils/port/IPAddress.h"
#include "ton/ton-types.h"
#include "adnl/adnl-node-id.hpp"

#include <map>
#include <deque>
#include <vector>
#include <mutex>
#include <atomic>

namespace ton {
namespace listener {

// Статистика получения блока
struct BlockReceptionStats {
  BlockIdExt block_id;                   // Идентификатор блока
  std::string source_id;                 // Идентификатор источника (узла)
  td::Timestamp received_at;             // Время получения
  size_t message_size;                   // Размер сообщения
  std::string source_addr;               // IP адрес источника
  double processing_time{0.0};           // Время обработки в секундах

  // Дополнительные метрики
  int shard_workchain{0};                // Номер воркчейна
  int validation_status{0};              // Статус валидации (0=неизвестно, 1=валидный, 2=невалидный)

  std::string to_json() const {
    std::string result = "{\n";
    result += "  \"block_id\": \"" + block_id.to_str() + "\",\n";
    result += "  \"source_id\": \"" + source_id + "\",\n";
    result += "  \"received_at\": " + std::to_string(received_at.at()) + ",\n";
    result += "  \"message_size\": " + std::to_string(message_size) + ",\n";
    result += "  \"source_addr\": \"" + source_addr + "\",\n";
    result += "  \"processing_time\": " + std::to_string(processing_time) + ",\n";
    result += "  \"shard_workchain\": " + std::to_string(shard_workchain) + ",\n";
    result += "  \"validation_status\": " + std::to_string(validation_status) + "\n";
    result += "}";
    return result;
  }
};

// Трекер приема блоков - отслеживает статистику и хранит историю
class BlockReceptionTracker {
 public:
  // Основной метод для отслеживания полученного блока
  void track_block_received(BlockIdExt block_id, std::string source_id,
                            td::Timestamp received_at, size_t message_size,
                            std::string source_addr = "",
                            double processing_time = 0.0) {
    std::lock_guard<std::mutex> guard(mutex_);

    // Создаем запись статистики
    BlockReceptionStats stats;
    stats.block_id = block_id;
    stats.source_id = source_id;
    stats.received_at = received_at;
    stats.message_size = message_size;
    stats.source_addr = source_addr;
    stats.processing_time = processing_time;

    // Дополнительные поля
    if (block_id.is_valid()) {
      stats.shard_workchain = block_id.id.workchain;
    }

    // Сохраняем в обоих хранилищах
    recent_blocks_[block_id.to_str()] = stats;
    reception_history_.push_back(stats);

    // Обновляем агрегированную статистику
    blocks_received_++;
    total_bytes_received_ += message_size;

    if (!reception_history_.empty()) {
      double alpha = 0.1; // Коэффициент сглаживания
      avg_processing_time_ = alpha * processing_time + (1.0 - alpha) * avg_processing_time_;
    }

    // Ограничиваем размер истории
    if (reception_history_.size() > max_history_size_) {
      reception_history_.pop_front();
    }

    // Обновляем статистику по воркчейнам
    if (block_id.is_valid()) {
      workchain_stats_[block_id.id.workchain]++;
    }

    // Если с момента последнего лога прошло достаточно времени, логируем статистику
    auto now = td::Timestamp::now();
    if (now.at() - last_log_time_.at() > 60.0) { // Каждую минуту
      LOG(INFO) << "BlockReceptionTracker: " << blocks_received_ << " blocks received, "
                << "avg processing time: " << avg_processing_time_ << "s";
      last_log_time_ = now;
    }
  }

  // Получить статистику по последним блокам
  std::vector<BlockReceptionStats> get_recent_blocks_stats(int limit = 100) const {
    std::lock_guard<std::mutex> guard(mutex_);

    std::vector<BlockReceptionStats> result;
    result.reserve(std::min(reception_history_.size(), static_cast<size_t>(limit)));

    int count = 0;
    for (auto it = reception_history_.rbegin(); it != reception_history_.rend() && count < limit; ++it, ++count) {
      result.push_back(*it);
    }

    return result;
  }

  // Получить детали по конкретному блоку
  BlockReceptionStats get_block_stats(const std::string& block_id_str) const {
    std::lock_guard<std::mutex> guard(mutex_);

    auto it = recent_blocks_.find(block_id_str);
    if (it != recent_blocks_.end()) {
      return it->second;
    }

    // Возвращаем пустую статистику если блок не найден
    BlockReceptionStats empty;
    return empty;
  }

  // Общая статистика
  double get_average_processing_time() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return avg_processing_time_;
  }

  size_t get_blocks_received_count() const {
    return blocks_received_.load();
  }

  size_t get_total_bytes_received() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return total_bytes_received_;
  }

  // Получить статистику по воркчейнам
  std::map<int, int> get_workchain_stats() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return workchain_stats_;
  }

  // Получить полную статистику в виде JSON
  std::string get_full_stats_json() const {
    std::lock_guard<std::mutex> guard(mutex_);

    std::string result = "{\n";
    result += "  \"blocks_received\": " + std::to_string(blocks_received_) + ",\n";
    result += "  \"total_bytes_received\": " + std::to_string(total_bytes_received_) + ",\n";
    result += "  \"avg_processing_time\": " + std::to_string(avg_processing_time_) + ",\n";

    // Статистика по воркчейнам
    result += "  \"workchain_stats\": {\n";
    bool first = true;
    for (const auto& pair : workchain_stats_) {
      if (!first) {
        result += ",\n";
      }
      result += "    \"" + std::to_string(pair.first) + "\": " + std::to_string(pair.second);
      first = false;
    }
    result += "\n  }\n";

    result += "}";
    return result;
  }

 private:
  std::map<std::string, BlockReceptionStats> recent_blocks_; // Блоки по ID
  std::deque<BlockReceptionStats> reception_history_;        // Хронологическая история
  std::atomic<size_t> blocks_received_{0};                   // Общее число блоков
  size_t total_bytes_received_{0};                           // Общее количество байт
  double avg_processing_time_{0.0};                          // Среднее время обработки
  std::map<int, int> workchain_stats_;                       // Статистика по воркчейнам
  td::Timestamp last_log_time_ = td::Timestamp::now();       // Время последнего лога

  static constexpr size_t max_history_size_ = 10000;         // Максимальный размер истории
  mutable std::mutex mutex_;                                 // Мьютекс для потокобезопасности
};

} // namespace listener
} // namespace ton
