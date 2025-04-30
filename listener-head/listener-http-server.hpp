#pragma once

#include "td/actor/actor.h"
#include "td/net/HttpServer.h"
#include "block-reception-tracker.hpp"
#include "td/utils/JsonBuilder.h"
#include <memory>

namespace ton {
namespace listener {

class ListenerHttpServer : public td::actor::Actor {
 public:
  ListenerHttpServer(td::uint16 port, td::Ref<BlockReceptionTracker> tracker)
      : port_(port), tracker_(std::move(tracker)) {
  }

  void handle_request(td::HttpQuery query, td::Promise<td::HttpServerResponse> promise) {
    td::HttpServerResponse response;
    response.http_version_ = query.http_version_;
    response.connection_keep_alive_ = query.connection_keep_alive_;
    response.content_type_ = "application/json";

    if (query.path_ == "/stats") {
      response.code_ = 200;
      response.message_ = "OK";
      response.content_ = get_stats_json();
    } else if (query.path_ == "/recent_blocks") {
      response.code_ = 200;
      response.message_ = "OK";
      response.content_ = get_recent_blocks_json(100);
    } else if (query.path_.substr(0, 12) == "/block_stats/") {
      auto block_id_str = query.path_.substr(12);
      response.code_ = 200;
      response.message_ = "OK";
      response.content_ = get_block_stats_json(block_id_str);
    } else {
      response.code_ = 404;
      response.message_ = "Not Found";
      response.content_ = "404 Not Found";
    }

    promise.set_value(std::move(response));
  }

 private:
  std::string get_stats_json() {
    td::JsonBuilder jb;
    auto json_obj = jb.enter_object();

    json_obj("blocks_received", tracker_->get_blocks_received_count());
    json_obj("avg_processing_time", tracker_->get_average_processing_time());

    auto stats = tracker_->get_recent_blocks_stats(10);
    auto blocks_arr = json_obj.enter_array("recent_blocks");

    for (const auto& stat : stats) {
      auto block_obj = blocks_arr.enter_object();
      block_obj("block_id", stat.block_id.to_str());
      block_obj("received_at", stat.received_at.at());
      block_obj("size", stat.message_size);
      block_obj("processing_time", stat.processing_time);
      block_obj.leave();
    }

    blocks_arr.leave();
    json_obj.leave();

    return jb.string_builder().as_cslice().str();
  }

  std::string get_recent_blocks_json(int limit) {
    td::JsonBuilder jb;
    auto json_obj = jb.enter_object();

    auto stats = tracker_->get_recent_blocks_stats(limit);
    auto blocks_arr = json_obj.enter_array("blocks");

    for (const auto& stat : stats) {
      auto block_obj = blocks_arr.enter_object();
      block_obj("block_id", stat.block_id.to_str());
      block_obj("received_at", stat.received_at.at());
      block_obj("source_node", stat.source_node.to_hex());
      block_obj("size", stat.message_size);
      block_obj("processing_time", stat.processing_time);
      block_obj.leave();
    }

    blocks_arr.leave();
    json_obj.leave();

    return jb.string_builder().as_cslice().str();
  }

  std::string get_block_stats_json(const std::string& block_id_str) {
    td::JsonBuilder jb;
    auto json_obj = jb.enter_object();

    // В реальной реализации добавьте функцию для парсинга BlockIdExt из строки
    BlockIdExt block_id;

    auto stats = tracker_->get_block_stats(block_id);

    json_obj("block_id", stats.block_id.to_str());
    json_obj("received_at", stats.received_at.at());
    json_obj("source_node", stats.source_node.to_hex());
    json_obj("source_addr", stats.source_addr.get_ip_str().str());
    json_obj("size", stats.message_size);
    json_obj("processing_time", stats.processing_time);

    json_obj.leave();

    return jb.string_builder().as_cslice().str();
  }

  void start_up() override {
    server_ = td::actor::create_actor<td::HttpServer>("http-server", port_,
                                                      [SelfId = actor_id(this)](td::HttpQuery query, td::Promise<td::HttpServerResponse> promise) {
                                                        td::actor::send_closure(SelfId, &ListenerHttpServer::handle_request, std::move(query), std::move(promise));
                                                      }
    );
    LOG(INFO) << "HTTP server started on port " << port_;
  }

  td::uint16 port_;
  td::Ref<BlockReceptionTracker> tracker_;
  td::actor::ActorOwn<td::HttpServer> server_;
};

} // namespace listener
} // namespace ton
