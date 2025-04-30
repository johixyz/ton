#pragma once

#include "td/actor/actor.h"
#include "http/http-server.h"
#include "http/http.h"
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

  void start_up() override {
    // Создаем callback для HTTP сервера
    class HttpCallback : public ton::http::HttpServer::Callback {
     public:
      HttpCallback(td::actor::ActorId<ListenerHttpServer> server_id)
          : server_id_(server_id) {
      }

      void receive_request(
          std::unique_ptr<ton::http::HttpRequest> request,
          std::shared_ptr<ton::http::HttpPayload> payload,
          td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                std::shared_ptr<ton::http::HttpPayload>>> promise) override {

        // Отправляем запрос на обработку
        td::actor::send_closure(server_id_, &ListenerHttpServer::handle_request, std::move(request),
                                std::move(payload), std::move(promise));
      }

     private:
      td::actor::ActorId<ListenerHttpServer> server_id_;
    };

    auto callback = std::make_shared<HttpCallback>(actor_id(this));
    server_ = ton::http::HttpServer::create(port_, std::move(callback));

    LOG(INFO) << "HTTP server started on port " << port_;
  }

  void handle_request(std::unique_ptr<ton::http::HttpRequest> request,
                      std::shared_ptr<ton::http::HttpPayload> payload,
                      td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                            std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Анализируем URL
    std::string url = request->url();

    // Извлекаем путь из URL
    std::string path = url;
    size_t pos = url.find('?');
    if (pos != std::string::npos) {
      path = url.substr(0, pos);
    }

    if (path == "/stats") {
      process_stats_request(std::move(promise));
    } else if (path == "/recent_blocks") {
      process_recent_blocks_request(std::move(promise));
    } else if (path.substr(0, 12) == "/block_stats/") {
      auto block_id_str = path.substr(12);
      process_block_stats_request(block_id_str, std::move(promise));
    } else {
      auto response = ton::http::HttpResponse::create("HTTP/1.1", 404, "Not Found", false, request->keep_alive()).move_as_ok();
      response->add_header(ton::http::HttpHeader{"Content-Type", "text/plain"});
      response->add_header(ton::http::HttpHeader{"Content-Length", "9"});
      response->complete_parse_header();

      auto payload = response->create_empty_payload().move_as_ok();
      payload->add_chunk(td::BufferSlice("Not Found"));
      payload->complete_parse();

      promise.set_value(std::make_pair(std::move(response), std::move(payload)));
    }
  }

 private:
  void process_stats_request(td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                   std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Создаем JSON
    std::string json_content = get_stats_json();

    // Создаем ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "application/json"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(json_content.size())});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(json_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  void process_recent_blocks_request(td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                           std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Создаем JSON
    std::string json_content = get_recent_blocks_json(100);

    // Создаем ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "application/json"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(json_content.size())});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(json_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  void process_block_stats_request(const std::string& block_id_str,
                                   td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                         std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Создаем JSON
    std::string json_content = get_block_stats_json(block_id_str);

    // Создаем ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "application/json"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(json_content.size())});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(json_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  std::string get_stats_json() {
    td::JsonBuilder jb;
    auto json_obj = jb.enter_object();

    json_obj("blocks_received", static_cast<td::int64>(tracker_->get_blocks_received_count()));
    json_obj("avg_processing_time", tracker_->get_average_processing_time());

    auto stats = tracker_->get_recent_blocks_stats(10);
    td::JsonBuilder blocks_jb;
    auto blocks_arr = blocks_jb.enter_array();

    for (const auto& stat : stats) {
      auto block_obj = blocks_arr.enter_object();
      block_obj("block_id", stat.block_id.to_str());
      block_obj("received_at", stat.received_at.at());
      block_obj("size", static_cast<td::int64>(stat.message_size));
      block_obj("processing_time", stat.processing_time);
      block_obj.leave();
    }

    blocks_arr.leave();
    json_obj("recent_blocks", blocks_jb.extract_string_unsafe());
    json_obj.leave();

    return jb.string_builder().as_cslice().str();
  }

  std::string get_recent_blocks_json(int limit) {
    td::JsonBuilder jb;
    auto json_obj = jb.enter_object();

    auto stats = tracker_->get_recent_blocks_stats(limit);
    td::JsonBuilder blocks_jb;
    auto blocks_arr = blocks_jb.enter_array();

    for (const auto& stat : stats) {
      auto block_obj = blocks_arr.enter_object();
      block_obj("block_id", stat.block_id.to_str());
      block_obj("received_at", stat.received_at.at());
      block_obj("source_node", stat.source_node.serialize());
      block_obj("size", static_cast<td::int64>(stat.message_size));
      block_obj("processing_time", stat.processing_time);
      block_obj.leave();
    }

    blocks_arr.leave();
    json_obj("blocks", blocks_jb.extract_string_unsafe());
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
    json_obj("source_node", stats.source_node.serialize());
    json_obj("source_addr", stats.source_addr.get_ip_str().str());
    json_obj("size", static_cast<td::int64>(stats.message_size));
    json_obj("processing_time", stats.processing_time);

    json_obj.leave();

    return jb.string_builder().as_cslice().str();
  }

  td::uint16 port_;
  td::Ref<BlockReceptionTracker> tracker_;
  td::actor::ActorOwn<ton::http::HttpServer> server_;
};

} // namespace listener
} // namespace ton