// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioUnitRun.h"

#include <AudioToolbox/AudioUnit.h>
#include <MacTypes.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    // Arbitrary OSStatus values protect raw failure preservation, not device-loss classification.
    constexpr ::OSStatus kUnknownStopFailure = -123456;
    constexpr ::OSStatus kLaterStopFailure = -123457;
    constexpr ::OSStatus kUnknownStartFailure = -123458;
    constexpr std::size_t kCallCapacity = 8U;
    constexpr std::size_t kStatusCapacity = 4U;

    enum class UnitRunFunction : std::uint8_t
    {
      Start,
      Stop,
    };

    struct UnitRunCall final
    {
      UnitRunFunction function = UnitRunFunction::Start;
      ::OSStatus status = ::noErr;
    };

    // Opaque test units are never dereferenced or passed to the SDK.
    struct UnitRunFake final
    {
      std::array<UnitRunCall, kCallCapacity> calls{};
      std::array<::OSStatus, kStatusCapacity> startStatuses{};
      std::array<::OSStatus, kStatusCapacity> stopStatuses{};
      std::size_t callCount = 0U;
      std::size_t startCount = 0U;
      std::size_t stopCount = 0U;

      void record(UnitRunFunction const function, ::OSStatus const status)
      {
        if (callCount < calls.size())
        {
          calls[callCount] = {.function = function, .status = status};
        }

        ++callCount;
      }

      ::OSStatus queuedStatus(std::array<::OSStatus, kStatusCapacity> const& statuses, std::size_t& index)
      {
        if (index >= statuses.size())
        {
          ++index;
          return static_cast<::OSStatus>(1);
        }

        auto const status = statuses[index];
        ++index;
        return status;
      }

      CoreAudioUnitRunOperations operations()
      {
        return {
          .start =
            [this](::AudioUnit const /*unit*/)
          {
            auto const status = queuedStatus(startStatuses, startCount);
            record(UnitRunFunction::Start, status);
            return status;
          },
          .stop =
            [this](::AudioUnit const /*unit*/)
          {
            auto const status = queuedStatus(stopStatuses, stopCount);
            record(UnitRunFunction::Stop, status);
            return status;
          },
        };
      }
    };
  } // namespace

  TEST_CASE("CoreAudioUnitRun - confirmed stop settles a start and idle stop is a no-op")
  {
    auto fake = UnitRunFake{};
    auto run = CoreAudioUnitRun{fake.operations()};
    ::AudioUnit unit = nullptr;

    CHECK_FALSE(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK(run.canReset());

    auto const idleStop = run.stop(unit);
    CHECK(idleStop == ::noErr);
    CHECK(fake.callCount == 0U);
    CHECK(fake.startCount == 0U);
    CHECK(fake.stopCount == 0U);

    auto const startStatus = run.start(unit);
    CHECK(startStatus == ::noErr);
    CHECK(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK_FALSE(run.canReset());
    REQUIRE(fake.callCount == 1U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 0U);
    CHECK(fake.calls[0].function == UnitRunFunction::Start);
    CHECK(fake.calls[0].status == ::noErr);

    auto const stopStatus = run.stop(unit);
    CHECK(stopStatus == ::noErr);
    CHECK_FALSE(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK(run.canReset());
    REQUIRE(fake.callCount == 2U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 1U);
    CHECK(fake.calls[1].function == UnitRunFunction::Stop);
    CHECK(fake.calls[1].status == ::noErr);

    auto const repeatedStop = run.stop(unit);
    CHECK(repeatedStop == ::noErr);
    CHECK_FALSE(run.isStarted());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK(run.canReset());
    CHECK(fake.callCount == 2U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 1U);
  }

  TEST_CASE("CoreAudioUnitRun - a failed stop stays started and preserves the raw unconfirmed status")
  {
    auto fake = UnitRunFake{};
    fake.stopStatuses[0] = kUnknownStopFailure;
    auto run = CoreAudioUnitRun{fake.operations()};
    ::AudioUnit unit = nullptr;

    REQUIRE(run.start(unit) == ::noErr);
    auto const stopStatus = run.stop(unit);

    CHECK(stopStatus == kUnknownStopFailure);
    CHECK(run.isStarted());
    CHECK(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == kUnknownStopFailure);
    CHECK_FALSE(run.canStart());
    CHECK_FALSE(run.canReset());
    REQUIRE(fake.callCount == 2U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 1U);
    CHECK(fake.calls[0].function == UnitRunFunction::Start);
    CHECK(fake.calls[0].status == ::noErr);
    CHECK(fake.calls[1].function == UnitRunFunction::Stop);
    CHECK(fake.calls[1].status == kUnknownStopFailure);
  }

  TEST_CASE("CoreAudioUnitRun - later explicit stops retain failure until native success")
  {
    auto fake = UnitRunFake{};
    fake.stopStatuses[0] = kUnknownStopFailure;
    fake.stopStatuses[1] = kLaterStopFailure;
    fake.stopStatuses[2] = ::noErr;
    auto run = CoreAudioUnitRun{fake.operations()};
    ::AudioUnit unit = nullptr;

    REQUIRE(run.start(unit) == ::noErr);
    auto const firstStop = run.stop(unit);
    CHECK(firstStop == kUnknownStopFailure);
    CHECK(run.isStarted());
    CHECK(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == kUnknownStopFailure);
    CHECK_FALSE(run.canStart());
    CHECK_FALSE(run.canReset());
    CHECK(fake.stopCount == 1U);

    auto const secondStop = run.stop(unit);
    CHECK(secondStop == kLaterStopFailure);
    CHECK(run.isStarted());
    CHECK(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == kLaterStopFailure);
    CHECK_FALSE(run.canStart());
    CHECK_FALSE(run.canReset());
    REQUIRE(fake.callCount == 3U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 2U);
    CHECK(fake.calls[1].function == UnitRunFunction::Stop);
    CHECK(fake.calls[1].status == kUnknownStopFailure);
    CHECK(fake.calls[2].function == UnitRunFunction::Stop);
    CHECK(fake.calls[2].status == kLaterStopFailure);

    auto const confirmedStop = run.stop(unit);
    CHECK(confirmedStop == ::noErr);
    CHECK_FALSE(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK(run.canReset());
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 3U);
    REQUIRE(fake.callCount == 4U);
    CHECK(fake.calls[3].function == UnitRunFunction::Stop);
    CHECK(fake.calls[3].status == ::noErr);

    auto const restarted = run.start(unit);
    CHECK(restarted == ::noErr);
    CHECK(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK_FALSE(run.canReset());
    REQUIRE(fake.callCount == 5U);
    CHECK(fake.startCount == 2U);
    CHECK(fake.stopCount == 3U);
    CHECK(fake.calls[4].function == UnitRunFunction::Start);
    CHECK(fake.calls[4].status == ::noErr);
  }

  TEST_CASE("CoreAudioUnitRun - a failed start is not started and stop does not call native stop")
  {
    auto fake = UnitRunFake{};
    fake.startStatuses[0] = kUnknownStartFailure;
    auto run = CoreAudioUnitRun{fake.operations()};
    ::AudioUnit unit = nullptr;

    auto const startStatus = run.start(unit);
    CHECK(startStatus == kUnknownStartFailure);
    CHECK_FALSE(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK(run.canReset());
    REQUIRE(fake.callCount == 1U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 0U);
    CHECK(fake.calls[0].function == UnitRunFunction::Start);
    CHECK(fake.calls[0].status == kUnknownStartFailure);

    auto const stopStatus = run.stop(unit);
    CHECK(stopStatus == ::noErr);
    CHECK_FALSE(run.isStarted());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(fake.callCount == 1U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 0U);
  }

  TEST_CASE("CoreAudioUnitRun - detach clears status without native calls and permits the next start")
  {
    auto fake = UnitRunFake{};
    fake.stopStatuses[0] = kUnknownStopFailure;
    auto run = CoreAudioUnitRun{fake.operations()};
    ::AudioUnit unit = nullptr;

    REQUIRE(run.start(unit) == ::noErr);
    REQUIRE(run.stop(unit) == kUnknownStopFailure);
    REQUIRE(fake.callCount == 2U);
    REQUIRE(fake.startCount == 1U);
    REQUIRE(fake.stopCount == 1U);

    run.detach();

    CHECK_FALSE(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK(run.canReset());
    CHECK(fake.callCount == 2U);
    CHECK(fake.startCount == 1U);
    CHECK(fake.stopCount == 1U);
    CHECK(fake.calls[0].function == UnitRunFunction::Start);
    CHECK(fake.calls[0].status == ::noErr);
    CHECK(fake.calls[1].function == UnitRunFunction::Stop);
    CHECK(fake.calls[1].status == kUnknownStopFailure);

    auto const nextStart = run.start(unit);
    CHECK(nextStart == ::noErr);
    CHECK(run.isStarted());
    CHECK_FALSE(run.hasUnconfirmedStop());
    CHECK(run.lastStopStatus() == ::noErr);
    CHECK(run.canStart());
    CHECK_FALSE(run.canReset());
    REQUIRE(fake.callCount == 3U);
    CHECK(fake.startCount == 2U);
    CHECK(fake.stopCount == 1U);
    CHECK(fake.calls[2].function == UnitRunFunction::Start);
    CHECK(fake.calls[2].status == ::noErr);
  }
} // namespace ao::audio::backend::detail::test
