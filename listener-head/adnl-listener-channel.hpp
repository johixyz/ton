#pragma once

#include "adnl/adnl-channel.hpp"

namespace ton {
namespace listener {

// Расширенная структура пакета с временем получения
struct TimestampedAdnlPacket : public adnl::AdnlPacket {
  TimestampedAdnlPacket(adnl::AdnlPacket packet, td::Timestamp received_at)
      : adnl::AdnlPacket(std::move(packet)), received_at_(received_at) {}

  td::Timestamp received_at_;
};

// Модифицированный класс AdnlChannelImpl для отслеживания времени получения
class ListenerAdnlChannelImpl : public adnl::AdnlChannelImpl {
 public:
  using adnl::AdnlChannelImpl::AdnlChannelImpl;

  void receive(td::IPAddress addr, td::BufferSlice data) override {
    // Записываем время получения пакета
    auto reception_time = td::Timestamp::now();

    auto P = td::PromiseCreator::lambda([peer = peer_pair_, channel_id = channel_in_id_, addr, id = print_id(),
                                         size = data.size(), reception_time](td::Result<adnl::AdnlPacket> R) {
      if (R.is_error()) {
        VLOG(ADNL_WARNING) << id << ": dropping IN message: can not decrypt: " << R.move_as_error();
      } else {
        auto packet = R.move_as_ok();
        packet.set_remote_addr(addr);

        // В полной реализации здесь можно использовать расширенный пакет
        // TimestampedAdnlPacket ts_packet(std::move(packet), reception_time);

        td::actor::send_closure(peer, &adnl::AdnlPeerPair::receive_packet_from_channel,
                                channel_id, std::move(packet), size);
      }
    });

    decrypt(std::move(data), std::move(P));
  }
};

// Фабрика для создания модифицированного канала
class ListenerAdnlChannel : public adnl::AdnlChannel {
 public:
  static td::Result<td::actor::ActorOwn<AdnlChannel>> create(
      privkeys::Ed25519 pk, pubkeys::Ed25519 pub,
      adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
      adnl::AdnlChannelIdShort &out_id, adnl::AdnlChannelIdShort &in_id,
      td::actor::ActorId<adnl::AdnlPeerPair> peer_pair) {

    td::Ed25519::PublicKey pub_k = pub.export_key();
    td::Ed25519::PrivateKey priv_k = pk.export_key();

    TRY_RESULT_PREFIX(shared_secret, td::Ed25519::compute_shared_secret(pub_k, priv_k),
                      "failed to compute channel shared secret: ");
    CHECK(shared_secret.length() == 32);

    td::SecureString rev_secret{32};
    for (td::uint32 i = 0; i < 32; i++) {
      rev_secret.as_mutable_slice()[i] = shared_secret[31 - i];
    }

    auto R = [&]() -> std::pair<PrivateKey, PublicKey> {
      if (local_id < peer_id) {
        return {privkeys::AES{std::move(shared_secret)}, pubkeys::AES{std::move(rev_secret)}};
      } else if (peer_id < local_id) {
        return {privkeys::AES{std::move(rev_secret)}, pubkeys::AES{std::move(shared_secret)}};
      } else {
        auto c = shared_secret.copy();
        return {privkeys::AES{std::move(c)}, pubkeys::AES{std::move(shared_secret)}};
      }
    }();

    in_id = adnl::AdnlChannelIdShort{R.first.compute_short_id()};
    out_id = adnl::AdnlChannelIdShort{R.second.compute_short_id()};

    TRY_RESULT_PREFIX(encryptor, R.second.create_encryptor(), "failed to init channel encryptor: ");
    TRY_RESULT_PREFIX(decryptor, R.first.create_decryptor(), "failed to init channel decryptor: ");

    return td::actor::create_actor<ListenerAdnlChannelImpl>(
        "listener-channel", local_id, peer_id, peer_pair, in_id, out_id,
        std::move(encryptor), std::move(decryptor));
  }
};

} // namespace listener
}