// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>

extern "C"
{
#include <pipewire/core.h>
#include <pipewire/properties.h>
#include <pipewire/thread-loop.h>
}

#include <ctime>
#include <string_view>

namespace ao::audio::backend::detail
{
  // Called with the stream core's thread loop locked, before creating the stream.
  // Shares the stream negotiation's absolute deadline. Only a completed lookup
  // can leave a reroutable named target to native policy.
  Result<> setPipeWireNamedTarget(::pw_core& core,
                                  ::pw_thread_loop& loop,
                                  ::timespec const& deadline,
                                  ::pw_properties& properties,
                                  std::string_view deviceId,
                                  OutputRoutingPolicy routingPolicy,
                                  bool exclusive);
} // namespace ao::audio::backend::detail
