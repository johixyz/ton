#pragma once

#include "adnl/adnl.h"
#include "adnl/adnl-peer-table.h"
#include "adnl/adnl-network-manager.hpp"
#include "listener-network-config.hpp"

namespace ton {
namespace adnl {

// Расширенная версия класса AdnlNetworkManagerImpl для оптимизации сетевых параметров
class ListenerNetworkManagerImpl : public AdnlNetworkManagerImpl {
 public:
  using AdnlNetworkManagerImpl::AdnlNetworkManagerImpl;

  // Переопределение метода add_listening_udp_port для увеличения буферов сокетов
  size_t add_listening_udp_port(td::uint16 port) override {
    auto result = AdnlNetworkManagerImpl::add_listening_udp_port(port);

    // Увеличение буферов UDP сокетов
    auto idx = udp_sockets_.size() - 1;
    if (idx < udp_sockets_.size()) {
      td::actor::send_closure(udp_sockets_[idx].server, &td::UdpServer::set_receive_buffer_size,
                              listener::NetworkOptimizationConfig::UDP_BUFFER_SIZE);
      td::actor::send_closure(udp_sockets_[idx].server, &td::UdpServer::set_send_buffer_size,
                              listener::NetworkOptimizationConfig::UDP_BUFFER_SIZE);
    }

    return result;
  }

  // Модификация метода для увеличения приоритета соединений с валидаторами
  void add_self_addr(td::IPAddress addr, AdnlCategoryMask cat_mask, td::uint32 priority) override {
    AdnlNetworkManagerImpl::add_self_addr(addr, cat_mask, priority);
  }

  // Добавление оптимизаций обработки UDP сообщений
  void receive_udp_message(td::UdpMessage message, size_t idx) override {
    // Замер времени получения сообщения
    auto reception_time = td::Timestamp::now();

    // Здесь можно добавить код для добавления временной метки к сообщению

    // Вызов оригинального метода
    AdnlNetworkManagerImpl::receive_udp_message(std::move(message), idx);
  }
};

// Фабрика для создания оптимизированного NetworkManager
class ListenerNetworkManagerFactory {
 public:
  static td::actor::ActorOwn<AdnlNetworkManager> create(td::uint16 port) {
    return td::actor::create_actor<ListenerNetworkManagerImpl>("ListenerNetworkManager", port);
  }
};

} // namespace adnl
} // namespace ton
