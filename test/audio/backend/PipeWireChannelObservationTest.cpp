// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/PipeWireBackend.h"
#include "test/audio/BackendTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Device.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace ao::audio::backend::test
{
  TEST_CASE("PipeWireBackend - native client negotiation retains ordinary and alternate two-speaker offers",
            "[audio][integration][pipewire][.manual]")
  {
    // Use an explicitly selected existing route; do not provision server sinks,
    // change system defaults or start source rendering. Open creates its own
    // client stream and temporarily activates it with render admission closed.
    auto const* deviceId = std::getenv("AOBUS_TEST_PIPEWIRE_DEVICE_ID");

    if (deviceId == nullptr || *deviceId == '\0')
    {
      SKIP("Select an existing PipeWire sink with AOBUS_TEST_PIPEWIRE_DEVICE_ID");
    }

    auto const device = Device{.id = DeviceId{std::string{deviceId}},
                               .displayName = "Selected native PipeWire observation endpoint",
                               .backendId = kBackendPipeWire};
    auto target = audio::test::NoopRenderTarget{};
    auto backend = PipeWireBackend{device, kProfileShared};

    // Literal independent native position-set controls: FL/FR and BL/BR.
    for (auto const mask : std::to_array<std::uint32_t>({0x3, 0x30}))
    {
      CAPTURE(mask);
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = mask};
      auto const openedRes = backend.open(source, target);
      INFO((openedRes ? "native client offer accepted" : openedRes.error().error.message));
      REQUIRE(openedRes);
      CHECK(openedRes->clientFormat.sampleRate == 48000);
      CHECK(openedRes->clientFormat.channels == 2);
      CHECK(openedRes->clientFormat.speakerMask == mask);
      CHECK(openedRes->channelDelivery.isMapped);
      CHECK(openedRes->channelDelivery.nativeSpeakerMask == mask);
      CHECK_FALSE(openedRes->optEndpoint);

      backend.close();
    }
  }
} // namespace ao::audio::backend::test
