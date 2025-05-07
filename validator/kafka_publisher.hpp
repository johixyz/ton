#pragma once

#include "validator/validator.h"
#include "ton/ton-types.h"
#include <string>
#include <memory>

// Forward declarations for librdkafka
typedef struct rd_kafka_s rd_kafka_t;
typedef struct rd_kafka_topic_s rd_kafka_topic_t;

namespace ton {
namespace validator {

class KafkaPublisher {
 public:
  KafkaPublisher(std::string brokers, std::string blocks_topic_name, std::string unvalidated_blocks_topic_name);
  ~KafkaPublisher();
  
  // Publishes block information to Kafka
  void publish_block(BlockHandle handle, td::Ref<ShardState> state);
  void publish_unvalidated_block(const BlockBroadcast& broadcast); // Add this method


  
  // Checks if the publisher is properly initialized
  bool is_initialized() const { return producer_ != nullptr && blocks_topic_ != nullptr; }

 private:
  // Kafka producer instance
  rd_kafka_t* producer_{nullptr};
  
  // Kafka topic for blocks
  rd_kafka_topic_t* blocks_topic_{nullptr};
  rd_kafka_topic_t* unvalidated_blocks_topic_{nullptr};
  
  // Topic name
  std::string blocks_topic_name_;
  std::string unvalidated_blocks_topic_name_;

  // Serializes block data to JSON format
  std::string serialize_block(BlockHandle handle, td::Ref<ShardState> state);
  
  // Internal error logging function
  void log_error(const std::string& message);
  
  // Disallow copying
  KafkaPublisher(const KafkaPublisher&) = delete;
  KafkaPublisher& operator=(const KafkaPublisher&) = delete;
};

} // namespace validator
} // namespace ton
