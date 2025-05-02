#pragma once

#include "td/actor/actor.h"
#include "http/http-server.h"
#include "http/http.h"
#include "block-reception-tracker.hpp"
#include <memory>

namespace ton {
namespace listener {

// HTTP сервер для предоставления API мониторинга блоков
class ListenerHttpServer : public td::actor::Actor {
 public:
  ListenerHttpServer(td::uint16 port, std::shared_ptr<BlockReceptionTracker> tracker)
      : port_(port), tracker_(std::move(tracker)) {
  }

  void start_up() override {
    LOG(INFO) << "Starting HTTP server on port " << port_;

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

    LOG(INFO) << "HTTP server started successfully on port " << port_;
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

    LOG(DEBUG) << "Handling HTTP request: " << path;

    // Обрабатываем различные эндпоинты API
    if (path == "/api/stats") {
      process_stats_request(std::move(promise));
    } else if (path == "/api/recent_blocks") {
      process_recent_blocks_request(std::move(promise));
    } else if (path.substr(0, 16) == "/api/block_stats/") {
      auto block_id_str = path.substr(16);
      process_block_stats_request(block_id_str, std::move(promise));
    } else if (path == "/api/workchain_stats") {
      process_workchain_stats_request(std::move(promise));
    } else if (path == "/") {
      // Корневой эндпоинт - выдаем HTML страницу дашборда
      process_dashboard_request(std::move(promise));
    } else {
      // Обработка неизвестного пути
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
  // Обработка запроса общей статистики
  void process_stats_request(td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                   std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Создаем JSON с общей статистикой
    std::string json_content = tracker_->get_full_stats_json();

    // Создаем HTTP ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "application/json"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(json_content.size())});
    response->add_header(ton::http::HttpHeader{"Access-Control-Allow-Origin", "*"});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(json_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  // Обработка запроса последних блоков
  void process_recent_blocks_request(td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                           std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Получаем статистику по последним блокам
    auto stats = tracker_->get_recent_blocks_stats(100);

    // Формируем JSON ответ вручную
    std::string json_content = "{\n  \"blocks\": [\n";

    for (size_t i = 0; i < stats.size(); ++i) {
      json_content += "    " + stats[i].to_json();
      if (i < stats.size() - 1) {
        json_content += ",";
      }
      json_content += "\n";
    }

    json_content += "  ],\n";
    json_content += "  \"total\": " + std::to_string(stats.size()) + "\n";
    json_content += "}\n";

    // Создаем HTTP ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "application/json"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(json_content.size())});
    response->add_header(ton::http::HttpHeader{"Access-Control-Allow-Origin", "*"});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(json_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  // Обработка запроса статистики по конкретному блоку
  void process_block_stats_request(const std::string& block_id_str,
                                   td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                         std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Получаем статистику по конкретному блоку
    auto stats = tracker_->get_block_stats(block_id_str);

    // Формируем JSON ответ
    std::string json_content = stats.to_json();

    // Создаем HTTP ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "application/json"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(json_content.size())});
    response->add_header(ton::http::HttpHeader{"Access-Control-Allow-Origin", "*"});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(json_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  // Обработка запроса статистики по воркчейнам
  void process_workchain_stats_request(td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                             std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Получаем статистику по воркчейнам
    auto workchain_stats = tracker_->get_workchain_stats();

    // Формируем JSON ответ
    std::string json_content = "{\n  \"workchain_stats\": {\n";

    bool first = true;
    for (const auto& pair : workchain_stats) {
      if (!first) {
        json_content += ",\n";
      }
      json_content += "    \"" + std::to_string(pair.first) + "\": " + std::to_string(pair.second);
      first = false;
    }

    json_content += "\n  }\n}\n";

    // Создаем HTTP ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "application/json"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(json_content.size())});
    response->add_header(ton::http::HttpHeader{"Access-Control-Allow-Origin", "*"});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(json_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  // Обработка запроса дашборда
  void process_dashboard_request(td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>,
                                                       std::shared_ptr<ton::http::HttpPayload>>> promise) {
    // Создаем простую HTML страницу с дашбордом
    std::string html_content = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>TON Listener Head Dashboard</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f5f5f5; }
        .container { max-width: 1200px; margin: 0 auto; }
        .card { background-color: white; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
        h1, h2 { color: #333; }
        pre { background-color: #f8f8f8; padding: 15px; border-radius: 5px; overflow-x: auto; }
        table { width: 100%; border-collapse: collapse; }
        th, td { text-align: left; padding: 12px; border-bottom: 1px solid #ddd; }
        th { background-color: #f2f2f2; }
        tr:hover { background-color: #f5f5f5; }
        .stats-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(250px, 1fr)); gap: 20px; margin-bottom: 20px; }
        .stat-card { background-color: white; border-radius: 8px; padding: 20px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
        .stat-value { font-size: 24px; font-weight: bold; margin-top: 10px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>TON Listener Head Dashboard</h1>

        <div class="card">
            <h2>Block Reception Statistics</h2>
            <div class="stats-grid" id="stats-grid">
                <div class="stat-card">
                    <div>Total Blocks Received</div>
                    <div class="stat-value" id="blocks-received">Loading...</div>
                </div>
                <div class="stat-card">
                    <div>Total Data Volume</div>
                    <div class="stat-value" id="total-bytes">Loading...</div>
                </div>
                <div class="stat-card">
                    <div>Average Processing Time</div>
                    <div class="stat-value" id="avg-processing-time">Loading...</div>
                </div>
            </div>
        </div>

        <div class="card">
            <h2>Recent Blocks</h2>
            <table id="recent-blocks-table">
                <thead>
                    <tr>
                        <th>Block ID</th>
                        <th>Received At</th>
                        <th>Size</th>
                        <th>Processing Time</th>
                    </tr>
                </thead>
                <tbody id="recent-blocks-body">
                    <tr>
                        <td colspan="4">Loading...</td>
                    </tr>
                </tbody>
            </table>
        </div>

        <div class="card">
            <h2>Workchain Statistics</h2>
            <div id="workchain-stats">Loading...</div>
        </div>
    </div>

    <script>
        // Function to update dashboard data
        function updateDashboard() {
            // Fetch general statistics
            fetch('/api/stats')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('blocks-received').textContent = data.blocks_received.toLocaleString();
                    document.getElementById('total-bytes').textContent = formatBytes(data.total_bytes_received);
                    document.getElementById('avg-processing-time').textContent = data.avg_processing_time.toFixed(6) + ' sec';

                    // Update workchain stats
                    let wcStatsHtml = '<table>';
                    wcStatsHtml += '<tr><th>Workchain</th><th>Blocks Count</th></tr>';

                    for (const [workchain, count] of Object.entries(data.workchain_stats)) {
                        wcStatsHtml += `<tr><td>${workchain}</td><td>${count.toLocaleString()}</td></tr>`;
                    }

                    wcStatsHtml += '</table>';
                    document.getElementById('workchain-stats').innerHTML = wcStatsHtml;
                })
                .catch(error => console.error('Error fetching stats:', error));

            // Fetch recent blocks
            fetch('/api/recent_blocks')
                .then(response => response.json())
                .then(data => {
                    let tableHtml = '';

                    data.blocks.forEach(block => {
                        const date = new Date(block.received_at * 1000);
                        tableHtml += `<tr>
                            <td>${block.block_id}</td>
                            <td>${date.toLocaleString()}</td>
                            <td>${formatBytes(block.message_size)}</td>
                            <td>${block.processing_time.toFixed(6)} sec</td>
                        </tr>`;
                    });

                    if (tableHtml === '') {
                        tableHtml = '<tr><td colspan="4">No blocks received yet</td></tr>';
                    }

                    document.getElementById('recent-blocks-body').innerHTML = tableHtml;
                })
                .catch(error => console.error('Error fetching recent blocks:', error));
        }

        // Helper function to format bytes
        function formatBytes(bytes, decimals = 2) {
            if (bytes === 0) return '0 Bytes';

            const k = 1024;
            const dm = decimals < 0 ? 0 : decimals;
            const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB'];

            const i = Math.floor(Math.log(bytes) / Math.log(k));

            return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
        }

        // Update the dashboard when page loads
        updateDashboard();

        // Update every 10 seconds
        setInterval(updateDashboard, 10000);
    </script>
</body>
</html>
    )";

    // Создаем HTTP ответ
    auto response = ton::http::HttpResponse::create("HTTP/1.1", 200, "OK", false, true).move_as_ok();
    response->add_header(ton::http::HttpHeader{"Content-Type", "text/html; charset=utf-8"});
    response->add_header(ton::http::HttpHeader{"Content-Length", std::to_string(html_content.size())});
    response->complete_parse_header();

    // Создаем payload
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice(html_content));
    payload->complete_parse();

    promise.set_value(std::make_pair(std::move(response), std::move(payload)));
  }

  td::uint16 port_;
  std::shared_ptr<BlockReceptionTracker> tracker_;
  td::actor::ActorOwn<ton::http::HttpServer> server_;
};

} // namespace listener
} // namespace ton
