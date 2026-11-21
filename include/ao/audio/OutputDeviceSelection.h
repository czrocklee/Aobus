// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PipeWireTargetIdentity.h>

#include <cstdint>

namespace ao::audio
{
  enum class OutputRoutingPolicy : std::uint8_t
  {
    Fixed,
    AllowSystemReroute,
  };

  /** One requested or configured route. An empty provider-defined default follows the system independently of this
   * policy. */
  struct OutputDeviceSelection final
  {
    BackendId backendId{};
    DeviceId deviceId{};
    ProfileId profileId{};
    OutputRoutingPolicy routingPolicy = OutputRoutingPolicy::Fixed;

    bool operator==(OutputDeviceSelection const&) const = default;
  };

  /// Can this request carry saved rerouting consent, even before discovery?
  /// The live provider must still accept it through createReroutableBackend().
  inline bool supportsOutputRoutingPolicy(OutputDeviceSelection const& selection) noexcept
  {
    return selection.backendId == kBackendPipeWire && pipeWireTargetNodeName(selection.deviceId.raw()).has_value() &&
           (selection.profileId == kProfileShared || selection.profileId == kProfileExclusive);
  }
} // namespace ao::audio
