// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <type_traits>

namespace ao::uimodel::test
{
  namespace
  {
    audio::OutputDeviceSelection makeSelection()
    {
      return {
        .backendId = audio::kBackendPipeWire,
        .deviceId = audio::DeviceId{"studio-dac"},
        .profileId = audio::kProfileExclusive,
      };
    }
  } // namespace

  TEST_CASE("OutputDeviceIntent - a destination cannot be left undecided", "[uimodel][unit][playback][output]")
  {
    // The whole point of the type: a bundle carrying one cannot be assembled
    // without saying where requested routes go, so a shell that forgets to
    // forward its recorder fails to build instead of dropping every selection.
    STATIC_CHECK_FALSE(std::is_default_constructible_v<OutputDeviceIntent>);
  }

  TEST_CASE("OutputDeviceIntent - records what a surface owns", "[uimodel][unit][playback][output]")
  {
    auto optRecorded = std::optional<audio::OutputDeviceSelection>{};
    auto const intent = OutputDeviceIntent::recordedBy([&optRecorded](audio::OutputDeviceSelection const& selection)
                                                       { optRecorded = selection; });

    intent.record(makeSelection());

    REQUIRE(optRecorded);
    CHECK(*optRecorded == makeSelection());
  }

  TEST_CASE("OutputDeviceIntent - a discarded destination keeps nothing", "[uimodel][unit][playback][output]")
  {
    auto const intent = OutputDeviceIntent::discarded();

    CHECK_NOTHROW(intent.record(makeSelection()));
  }

  TEST_CASE("OutputDeviceIntent - a copy records to the same destination", "[uimodel][unit][playback][output]")
  {
    // Component bundles are copied into each surface that borrows them, so a
    // copy must stay attached to the frontend's recorder.
    std::int32_t recordedCount = 0;
    auto const intent = OutputDeviceIntent::recordedBy([&recordedCount](auto const&) { ++recordedCount; });
    auto const borrowed = OutputDeviceIntent{intent};

    borrowed.record(makeSelection());
    intent.record(makeSelection());

    CHECK(recordedCount == 2);
  }
} // namespace ao::uimodel::test
