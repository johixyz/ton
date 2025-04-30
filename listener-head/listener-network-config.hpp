#pragma once

#include <limits>

namespace ton {
namespace listener {

struct NetworkOptimizationConfig {
  static constexpr size_t MAX_OUTBOUND_CONNECTIONS = 1000;
  static constexpr size_t MAX_INBOUND_CONNECTIONS = 5000;
  static constexpr size_t UDP_BUFFER_SIZE = 10 * 1024 * 1024; // 10 MB
  static constexpr td::uint32 VALIDATOR_PRIORITY = std::numeric_limits<td::uint32>::max();
  static constexpr double CONNECTION_RESET_INTERVAL = 3600.0;
  static constexpr double CONNECTION_TIMEOUT = 5.0;
  static constexpr double MAX_HEARTBEAT_INTERVAL = 10.0;
  static constexpr double MIN_HEARTBEAT_INTERVAL = 1.0;
};

} // namespace listener
} // namespace ton
