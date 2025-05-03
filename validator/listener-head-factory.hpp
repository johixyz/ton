// listener-head-factory.hpp
#pragma once

#include "validator/validator.h"
#include "adnl/adnl.h"
#include "rldp/rldp.h"
#include "overlay/overlay.h"

namespace ton {
namespace validator {
namespace listener {

class ListenerHeadFactory {
 public:
  static td::actor::ActorOwn<ValidatorManagerInterface> create(
      td::Ref<ValidatorManagerOptions> opts, std::string db_root,
      td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
      td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays);
};

} // namespace listener
} // namespace validator
} // namespace ton