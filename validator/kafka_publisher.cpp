#include "kafka_publisher.hpp"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include <librdkafka/rdkafka.h>

namespace ton {
namespace validator {

// Callback for Kafka errors
static void kafka_logger(const rd_kafka_t* rk, int level, const char* fac, const char* buf) {
  LOG(ERROR) << "KAFKA (" << level << "): " << fac << ": " << buf;
}

KafkaPublisher::KafkaPublisher(std::string brokers, std::string blocks_topic_name)
    : blocks_topic_name_(std::move(blocks_topic_name)) {

  char errstr[512];

  // Configure Kafka
  rd_kafka_conf_t* conf = rd_kafka_conf_new();

  // Set bootstrap servers
  if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    LOG(ERROR) << "Kafka configuration error: " << errstr;
    rd_kafka_conf_destroy(conf);
    return;
  }

  // Set error callback
  rd_kafka_conf_set_log_cb(conf, kafka_logger);

  // Create producer
  producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if (!producer_) {
    LOG(ERROR) << "Failed to create Kafka producer: " << errstr;
    rd_kafka_conf_destroy(conf);
    return;
  }

  // Configuration is now owned by producer

  // Create blocks topic
  blocks_topic_ = rd_kafka_topic_new(producer_, blocks_topic_name_.c_str(), nullptr);
  if (!blocks_topic_) {
    LOG(ERROR) << "Failed to create blocks topic: " << rd_kafka_err2str(rd_kafka_last_error());
    rd_kafka_destroy(producer_);
    producer_ = nullptr;
    return;
  }

  LOG(INFO) << "Kafka publisher initialized successfully for topic: " << blocks_topic_name_;
}

KafkaPublisher::~KafkaPublisher() {
  if (blocks_topic_) {
    rd_kafka_topic_destroy(blocks_topic_);
  }

  if (producer_) {
    // Wait for any outstanding messages to be delivered
    rd_kafka_flush(producer_, 5000); // 5 second timeout
    rd_kafka_destroy(producer_);
  }
}

void KafkaPublisher::publish_block(BlockHandle handle, td::Ref<ShardState> state) {
  if (!is_initialized()) {
    log_error("Kafka publisher not properly initialized");
    return;
  }

  // Serialize block data to JSON
  std::string message = serialize_block(handle, state);

  // Publish to Kafka
  int result = rd_kafka_produce(
      blocks_topic_,                // Topic
      RD_KAFKA_PARTITION_UA,        // Use default partitioner
      RD_KAFKA_MSG_F_COPY,          // Make a copy of the payload
      const_cast<char*>(message.data()), // Payload
      message.size(),               // Payload size
      nullptr,                      // Optional key
      0,                            // Key size
      nullptr                       // Message opaque
  );

  if (result == -1) {
    log_error("Failed to produce message: " + std::string(rd_kafka_err2str(rd_kafka_last_error())));
    return;
  }

  // Poll to handle delivery reports
  rd_kafka_poll(producer_, 0);
}

std::string KafkaPublisher::serialize_block(BlockHandle handle, td::Ref<ShardState> state) {
  td::JsonBuilder jb;
  auto json = jb.enter_object();

  // Block identification
  json("block_id", handle->id().to_str());
  json("workchain", static_cast<td::int32>(handle->id().id.workchain));
  json("shard", td::to_string(handle->id().id.shard));
  json("seqno", static_cast<td::int32>(handle->id().id.seqno));
  json("root_hash", td::base64_encode(handle->id().root_hash.as_slice()));
  json("file_hash", td::base64_encode(handle->id().file_hash.as_slice()));

  // Block metadata
  if (handle->inited_unix_time()) {
    json("unix_time", static_cast<td::int32>(handle->unix_time()));
  }
  if (handle->inited_is_key_block()) {
    // Use integers instead of booleans (booleans are not supported in JsonBuilder)
    json("is_key_block", handle->is_key_block() ? 1 : 0);
  }

  // Previous blocks
  if (handle->inited_prev_left()) {
    json("prev_block", handle->one_prev(true).to_str());
  }

  // If it's a merge block, add the second previous block
  if (handle->merge_before()) {
    json("prev_block_2", handle->one_prev(false).to_str());
  }

  // Basic state info
  if (state.not_null()) {
    // Create a new object with nested state info
    auto state_obj = jb.enter_object();
    json("state_info", td::JsonRaw("{}"));

    // Use integers (1/0) instead of booleans
    if (state->get_shard().is_masterchain()) {
      state_obj("is_masterchain", 1);
    } else {
      state_obj("is_masterchain", 0);
      state_obj("shard_full", state->get_shard().to_str());
    }
    state_obj("global_id", static_cast<td::int32>(state->get_global_id()));
    state_obj("seqno", static_cast<td::int32>(state->get_seqno()));

    state_obj("logical_time", static_cast<td::int64>(state->get_logical_time()));

    // Add masterchain block reference if this is a shardchain block
    if (!state->get_shard().is_masterchain()) {
      BlockIdExt mc_blkid = state->get_block_id();
      state_obj("referred_mc_block", mc_blkid.to_str());
    }
  }

  // Convert to string and return
  return jb.string_builder().as_cslice().str();
}

void KafkaPublisher::log_error(const std::string& message) {
  LOG(ERROR) << "KafkaPublisher error: " << message;
}

} // namespace validator
} // namespace ton