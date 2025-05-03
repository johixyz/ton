// listener-head-factory.cpp
#include "listener-head-factory.hpp"
#include "listener-head-manager.hpp"

namespace ton {
namespace validator {
namespace listener {

td::actor::ActorOwn<ValidatorManagerInterface> ListenerHeadFactory::create(
    td::Ref<ValidatorManagerOptions> opts, std::string db_root,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays) {

  return ListenerHeadManager::create(std::move(opts), db_root, keyring, adnl, rldp, overlays);
}

} // namespace listener
} // namespace validator
} // namespace ton