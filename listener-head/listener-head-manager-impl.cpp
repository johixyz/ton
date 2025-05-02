// listener-head-manager-impl.cpp

#include "listener-head-manager.hpp"
#include "td/utils/overloaded.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include "validator/impl/block.hpp"
#include "validator/interfaces/block.h"

namespace ton {
namespace listener {

// Callback для получения сообщений из overlay сети
class ListenerOverlayCallback : public overlay::Overlays::Callback {
 public:
  ListenerOverlayCallback(td::actor::ActorId<ListenerHeadManager> manager, overlay::OverlayIdShort overlay_id)
      : manager_(manager), overlay_id_(overlay_id) {}

  void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id,
                       td::BufferSlice data) override {
    td::actor::send_closure(manager_, &ListenerHeadManager::process_overlay_message,
                            src, overlay_id_, std::move(data));
  }

  void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id,
                     td::BufferSlice data, td::Promise<td::BufferSlice> promise) override {
    // Мы только слушаем, не отвечаем на запросы
    promise.set_error(td::Status::Error(ErrorCode::notready, "queries not supported by listener"));
  }

  void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id,
                         td::BufferSlice data) override {
    td::actor::send_closure(manager_, &ListenerHeadManager::process_broadcast_message,
                            src, overlay_id_, std::move(data));
  }

  // Метод для check_broadcast, всегда разрешаем
  void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::Unit> promise) override {
    promise.set_value(td::Unit());
  }

  // Для статистики
  void get_stats_extra(td::Promise<std::string> promise) override {
    promise.set_value("{}");
  }

 private:
  td::actor::ActorId<ListenerHeadManager> manager_;
  overlay::OverlayIdShort overlay_id_;
};

void ListenerHeadManager::start_up() {
  LOG(INFO) << "ListenerHeadManager starting up...";

  // Устанавливаем периодический таймер для действий
  alarm_timestamp() = td::Timestamp::in(1.0);

  start_listening();

  LOG(INFO) << "ListenerHeadManager started successfully";
}

void ListenerHeadManager::start_listening() {
  LOG(INFO) << "Starting to listen for blocks...";

  // Проверяем, есть ли заданные overlay IDs
  if (monitored_overlays_.empty()) {
    LOG(WARNING) << "No overlays to monitor, creating default overlay IDs";

    // Создаем overlay ID для мастерчейна (блоки)
    auto masterchain_blocks = create_tl_object<ton_api::tonNode_blockIdExt>(
        ton::masterchainId, ton::shardIdAll, 0, td::Bits256::zero(), td::Bits256::zero());
    auto masterchain_node_id_full = adnl::AdnlNodeIdFull{ton::PublicKey{ton::pubkeys::Ed25519{masterchain_blocks->root_hash_}}};
    auto masterchain_overlay_id = overlay::OverlayIdFull{masterchain_node_id_full.pubkey().export_as_slice()};

    // Создаем overlay ID для базового воркчейна (блоки)
    auto basechain_blocks = create_tl_object<ton_api::tonNode_blockIdExt>(
        ton::basechainId, ton::shardIdAll, 0, td::Bits256::zero(), td::Bits256::zero());

    auto basechain_node_id_full = adnl::AdnlNodeIdFull{ton::PublicKey{ton::pubkeys::Ed25519{masterchain_blocks->root_hash_}}};
    auto basechain_overlay_id = overlay::OverlayIdFull{basechain_node_id_full.pubkey().export_as_slice()};

    // Подписываемся на оверлеи
    add_overlay_to_listen(masterchain_overlay_id.compute_short_id());
    add_overlay_to_listen(basechain_overlay_id.compute_short_id());
  }
}

void ListenerHeadManager::start_listening_overlay(overlay::OverlayIdShort overlay_id) {
  LOG(INFO) << "Starting to listen for overlay: " << overlay_id.bits256_value();

  // Создаем callback для сообщений оверлея
  auto callback = std::make_unique<ListenerOverlayCallback>(actor_id(this), overlay_id);

  // Создаем настройки оверлея с флагами для приема блоков
  overlay::OverlayOptions options;
  options.max_neighbours_ = 256;  // Подключаемся к большому количеству пиров для лучшего приема
  options.announce_self_ = false; // Не анонсировать себя как валидатор

  // Устанавливаем флаги для типа участника оверлея (только прием)
  options.local_overlay_member_flags_ = 2; // Только получение блоков, без проверки

  // Подписываемся на оверлей через overlay manager
  td::actor::send_closure(overlays_, &overlay::Overlays::create_public_overlay,
                          local_id_, overlay_id, std::move(callback),
                          overlay::OverlayPrivacyRules::everybody(), "blocks", std::move(options));
}

void ListenerHeadManager::process_overlay_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id,
                                                  td::BufferSlice data) {
  auto reception_time = td::Timestamp::now();

  // Пытаемся разобрать сообщение как block-related message
  auto R = fetch_tl_object<ton_api::overlay_Broadcast>(data.clone(), true);
  if (R.is_error()) {
    LOG(DEBUG) << "Received non-block overlay message from " << src.bits256_value() << ", size: " << data.size();
    return;
  }

  auto obj = R.move_as_ok();

  td::ton_api::downcast_call(*obj.get(), [&, self = this, src, reception_time](auto& object) {
    using td::to_string;
    using namespace td::ton_api;

    LOG(DEBUG) << "Processing overlay message of type: " << to_string(object.get_id());

    if constexpr (std::is_same_v<std::decay_t<decltype(object)>, ton_api::overlay_broadcast>) {
      self->process_block_broadcast(src, reception_time, object);
    } else if constexpr (std::is_same_v<std::decay_t<decltype(object)>, ton_api::overlay_broadcastFec>) {
      self->process_block_broadcast_fec(src, reception_time, object);
    } else if constexpr (std::is_same_v<std::decay_t<decltype(object)>, ton_api::overlay_broadcastFecShort>) {
      self->process_block_broadcast_fec_short(src, reception_time, object);
    } else if constexpr (std::is_same_v<std::decay_t<decltype(object)>, ton_api::overlay_broadcastNotFound>) {
      // Игнорируем
      LOG(DEBUG) << "Received overlay_broadcastNotFound from " << src.bits256_value();
    } else if constexpr (std::is_same_v<std::decay_t<decltype(object)>, ton_api::overlay_unicast>) {
      // Обрабатываем unicast сообщение (может содержать блок)
      self->process_block_unicast(src, reception_time, object);
    } else {
      LOG(DEBUG) << "Unsupported overlay message type from " << src.bits256_value();
    }
  });
}

void ListenerHeadManager::process_broadcast_message(PublicKeyHash src, overlay::OverlayIdShort overlay_id,
                                                    td::BufferSlice data) {
  auto reception_time = td::Timestamp::now();

  LOG(DEBUG) << "Received broadcast message from " << src.bits256_value() << ", size: " << data.size();

  // Пытаемся разобрать как broadcast с блоком
  auto R = fetch_tl_object<ton_api::tonNode_blockBroadcast>(data.clone(), true);
  if (R.is_error()) {
    LOG(DEBUG) << "Received non-block broadcast message: " << R.error();
    return;
  }

  auto block_broadcast = R.move_as_ok();

  // Получаем BlockId из tl структуры
  BlockIdExt block_id;
  block_id.id.workchain = block_broadcast->id_->workchain_;
  block_id.id.shard = block_broadcast->id_->shard_;
  block_id.id.seqno = block_broadcast->id_->seqno_;
  block_id.root_hash.as_slice().copy_from(td::Slice(block_broadcast->id_->root_hash_.data(), 32));
  block_id.file_hash.as_slice().copy_from(td::Slice(block_broadcast->id_->file_hash_.data(), 32));

  LOG(INFO) << "Received block broadcast: " << block_id.to_str()
            << " from " << src.bits256_value().to_hex()
            << ", size: " << data.size();

  // Отслеживаем прием блока
  track_block_received(block_id, src.bits256_value().to_hex(), reception_time,
                       data.size(), "broadcast");

  // Здесь можно добавить дополнительную обработку блока
  try_process_block(block_id, std::move(block_broadcast->data_), src.bits256_value().to_hex());
}

void ListenerHeadManager::process_block_broadcast(adnl::AdnlNodeIdShort src, td::Timestamp reception_time,
                                                  ton_api::overlay_broadcast& msg) {
  LOG(DEBUG) << "Processing overlay_broadcast from " << src.bits256_value();

  // Пытаемся разобрать данные как broadcast с блоком
  auto R = fetch_tl_object<ton_api::tonNode_blockBroadcast>(msg.data_.clone(), true);
  if (R.is_error()) {
    // Возможно, это другой тип broadcast - проверяем blockUpdate
    auto R2 = fetch_tl_object<ton_api::tonNode_blockUpdate>(msg.data_.clone(), true);
    if (R2.is_error()) {
      LOG(DEBUG) << "Non-block broadcast message";
      return;
    }

    auto block_update = R2.move_as_ok();

    // Получаем BlockId из tl структуры
    BlockIdExt block_id;
    block_id.id.workchain = block_update->block_->workchain_;
    block_id.id.shard = block_update->block_->shard_;
    block_id.id.seqno = block_update->block_->seqno_;
    block_id.root_hash.as_slice().copy_from(td::Slice(block_update->block_->root_hash_.data(), 32));
    block_id.file_hash.as_slice().copy_from(td::Slice(block_update->block_->file_hash_.data(), 32));

    LOG(INFO) << "Received block update: " << block_id.to_str()
              << " from " << src.bits256_value().to_hex()
              << ", size: " << msg.data_.size();

    // Отслеживаем прием блока (без полных данных)
    track_block_received(block_id, src.bits256_value().to_hex(), reception_time,
                         msg.data_.size(), src.bits256_value().to_hex());
    return;
  }

  auto block_broadcast = R.move_as_ok();

  // Получаем BlockId из tl структуры
  BlockIdExt block_id;
  block_id.id.workchain = block_broadcast->id_->workchain_;
  block_id.id.shard = block_broadcast->id_->shard_;
  block_id.id.seqno = block_broadcast->id_->seqno_;
  block_id.root_hash.as_slice().copy_from(td::Slice(block_broadcast->id_->root_hash_.data(), 32));
  block_id.file_hash.as_slice().copy_from(td::Slice(block_broadcast->id_->file_hash_.data(), 32));

  LOG(INFO) << "Received block broadcast: " << block_id.to_str()
            << " from " << src.bits256_value().to_hex()
            << ", size: " << msg.data_.size();

  // Отслеживаем прием блока
  track_block_received(block_id, src.bits256_value().to_hex(), reception_time,
                       msg.data_.size(), src.bits256_value().to_hex());

  // Обработка полученного блока
  try_process_block(block_id, std::move(block_broadcast->data_), src.bits256_value().to_hex());
}

void ListenerHeadManager::process_block_broadcast_fec(adnl::AdnlNodeIdShort src, td::Timestamp reception_time,
                                                      ton_api::overlay_broadcastFec& msg) {
  LOG(DEBUG) << "Processing overlay_broadcastFec from " << src.bits256_value();

  // В FEC broadcast блок разбит на части, требуется восстановление
  // Здесь мы просто логируем прием, полная реализация требует FEC декодирования

  // Извлекаем hash как идентификатор broadcast
  td::Bits256 broadcast_hash = msg.broadcast_hash_;
  LOG(INFO) << "Received FEC broadcast part: " << broadcast_hash.to_hex()
            << " from " << src.bits256_value().to_hex()
            << ", size: " << msg.data_.size()
            << ", seqno: " << msg.seqno_
            << ", fec_type: " << msg.fec_type_->get_id();

  // Для полной реализации здесь нужно собирать части и восстанавливать блок
  // Это требует дополнительной реализации FEC декодирования
}

void ListenerHeadManager::process_block_broadcast_fec_short(adnl::AdnlNodeIdShort src, td::Timestamp reception_time,
                                                            ton_api::overlay_broadcastFecShort& msg) {
  LOG(DEBUG) << "Processing overlay_broadcastFecShort from " << src.bits256_value();

  // В FecShort используется сокращенный формат для экономии трафика
  // Аналогично broadcastFec, требуется восстановление

  td::Bits256 broadcast_hash = msg.broadcast_hash_;
  LOG(INFO) << "Received FEC short broadcast part: " << broadcast_hash.to_hex()
            << " from " << src.bits256_value().to_hex()
            << ", size: " << msg.data_.size()
            << ", seqno: " << msg.seqno_;
}

void ListenerHeadManager::process_block_unicast(adnl::AdnlNodeIdShort src, td::Timestamp reception_time,
                                                ton_api::overlay_unicast& msg) {
  LOG(DEBUG) << "Processing overlay_unicast from " << src.bits256_value();

  // Пытаемся разобрать как сообщение с блоком
  auto R = fetch_tl_object<ton_api::tonNode_data>(msg.data_.clone(), true);
  if (R.is_error()) {
    LOG(DEBUG) << "Non-block unicast message";
    return;
  }

  auto block_data = R.move_as_ok();
  LOG(INFO) << "Received block data via unicast"
            << " from " << src.bits256_value().to_hex()
            << ", size: " << msg.data_.size();

  // Для полной обработки нужно извлечь BlockId из данных,
  // но это требует дополнительного парсинга блока
}

void ListenerHeadManager::try_process_block(BlockIdExt block_id, td::BufferSlice data, std::string source) {
  auto reception_time = td::Timestamp::now();
  auto process_start = td::Timestamp::now();

  // Создаем BlockQ для минимальной обработки блока
  auto block_r = validator::BlockQ::create(block_id, std::move(data));
  if (block_r.is_error()) {
    LOG(WARNING) << "Failed to create BlockQ: " << block_r.error();
    return;
  }

  auto block = block_r.move_as_ok();

  // Получаем корневую ячейку для базовой проверки
  auto root_cell = block->root_cell();
  if (root_cell.is_null()) {
    LOG(WARNING) << "Block has null root cell";
    return;
  }

  // Рассчитываем время обработки
  auto process_end = td::Timestamp::now();
  double processing_time = process_end.at() - process_start.at();

  // Отслеживаем прием блока с включением данных о корневой ячейке
  track_block_received(block_id, source, reception_time, block->data().size(), source, processing_time);

  LOG(INFO) << "Processed block: " << block_id.to_str()
            << " from " << source
            << ", size: " << block->data().size()
            << ", processing time: " << processing_time << "s";
}

void ListenerHeadManager::add_overlay_to_listen(overlay::OverlayIdShort overlay_id) {
  LOG(INFO) << "Adding overlay to listen: " << overlay_id.bits256_value();

  // Проверяем, не добавлен ли уже этот оверлей
  if (monitored_overlays_.find(overlay_id) != monitored_overlays_.end()) {
    LOG(INFO) << "Overlay already being monitored";
    return;
  }

  monitored_overlays_.insert(overlay_id);
  start_listening_overlay(overlay_id);
}

void ListenerHeadManager::alarm() {
  // Периодические действия каждую минуту
  check_connection_status();

  // Переустанавливаем таймер
  alarm_timestamp() = td::Timestamp::in(60.0);
}

void ListenerHeadManager::check_connection_status() {
  // Проверка статуса соединений
  LOG(DEBUG) << "Checking connection status...";

  // Проверяем количество полученных блоков за последнее время
  auto now = td::Timestamp::now();
  if (blocks_received_ == last_blocks_received_count_) {
    // Если нет новых блоков, пытаемся переподключиться
    if (now.at() - last_block_received_at_.at() > 300.0) { // 5 минут без блоков
      LOG(WARNING) << "No new blocks received for 5 minutes, trying to reconnect...";
      // Перезапускаем подписки на оверлеи
      for (const auto& overlay_id : monitored_overlays_) {
        start_listening_overlay(overlay_id);
      }
      last_block_received_at_ = now;
    }
  } else {
    // Обновляем счетчик последнего полученного блока
    last_blocks_received_count_ = blocks_received_;
    last_block_received_at_ = now;
  }
}

void ListenerHeadManager::stop_listening() {
  // Отписываемся от всех оверлеев
  for (const auto& overlay_id : monitored_overlays_) {
    LOG(INFO) << "Stopping listening for overlay: " << overlay_id.bits256_value();
    // В TON SDK нет прямого метода для отписки, но можно удалить оверлей
    td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_id_, overlay_id);
  }
}

void ListenerHeadManager::add_known_validator(adnl::AdnlNodeIdShort validator_id, td::IPAddress addr) {
  LOG(INFO) << "Adding known validator: " << validator_id.bits256_value() << " at " << addr.get_ip_str();

  // Отправляем информацию в ConnectionManager
  td::actor::send_closure(connection_manager_, &ListenerConnectionManager::add_peer,
                          validator_id, addr, true);
}

} // namespace listener
} // namespace ton
