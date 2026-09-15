// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaMixerSession.h"

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "test/unit/audio/backend/AlsaMixerTestSupport.h"
#include <ao/Error.h>
#include <ao/utility/ThreadName.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <limits>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AlsaMixerSession - initialization repeat, failed reopen, and close never write shared mixer values",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawRange = {.min = 0L, .max = 255L}, .rawLevels = {255L}});

    REQUIRE(fixture.session.tryInit(nullptr));
    REQUIRE(fixture.session.tryInit(nullptr));
    fixture.statePtr->hardwareElements.front().active = false;
    CHECK_FALSE(fixture.session.tryInit(nullptr));
    fixture.session.close();

    CHECK(fixture.statePtr->openCount == 3U);
    CHECK(fixture.statePtr->closeCount == 3U);
    CHECK(fixture.statePtr->writeCount == 0U);
    CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{255L});
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::Unavailable);
  }

  TEST_CASE("AlsaMixerSession - candidate validation is read-only for mono and unbalanced channels",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};

    SECTION("mono")
    {
      fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {73L}});

      fixture.initialize();

      CHECK(fixture.session.stateSnapshot().volume == 0.73F);
      CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{73L});
    }

    SECTION("unbalanced channels")
    {
      fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L, 35L}});

      fixture.initialize();

      CHECK(fixture.session.stateSnapshot().volume == 0.8F);
      CHECK((fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{80L, 35L}));
    }

    CHECK(fixture.statePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaMixerSession - invalid preferred candidates are skipped without probing writes",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "Master", .index = 0U}, .readable = false});
    fixture.addElement({.id = {.name = "PCM", .index = 2U}, .rawRange = {.min = 12L, .max = 12L}});
    fixture.addElement({.id = {.name = "Digital", .index = 1U}, .rawLevels = {65L}});

    fixture.initialize();

    CHECK(fixture.session.stateSnapshot().volume == 0.65F);
    CHECK(fixture.statePtr->writeCount == 0U);
    CHECK(fixture.statePtr->hardwareElements[0].rawLevels == std::vector<long>{100L});
    CHECK(fixture.statePtr->hardwareElements[1].rawLevels == std::vector<long>{100L});
  }

  TEST_CASE("AlsaMixerSession - no valid candidate falls back without changing mixer values",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "Master", .index = 0U}, .active = false, .rawLevels = {44L}});
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {}});

    CHECK_FALSE(fixture.session.tryInit(nullptr));

    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::SoftwareGain);
    CHECK(fixture.statePtr->writeCount == 0U);
    CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{44L});
  }

  TEST_CASE("AlsaMixerSession - explicit raw volume writes use exact endpoints and fresh range",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement(
      {.id = {.name = "PCM", .index = 0U}, .rawRange = {.min = 10L, .max = 110L}, .rawLevels = {87L, 42L}});
    fixture.initialize();

    REQUIRE(fixture.session.setVolume(0.0F));
    REQUIRE(fixture.session.setVolume(0.5F));
    REQUIRE(fixture.session.setVolume(1.0F));

    CHECK((fixture.statePtr->writtenLevels == std::vector<long>{10L, 60L, 110L}));
    CHECK((fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{110L, 110L}));
    CHECK(fixture.session.stateSnapshot().volume == 1.0F);
    CHECK(fixture.statePtr->refreshCount == 5U);
  }

  TEST_CASE("AlsaMixerSession - explicit decibel volume writes use the readable decibel scale",
            "[audio][unit][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U},
                        .rawLevels = {80L},
                        .optDecibelRange = AlsaMixerLevelRange{.min = -6000L, .max = 0L},
                        .decibelLevels = {-1200L}});
    fixture.initialize();

    REQUIRE(fixture.session.setVolume(0.0F));
    REQUIRE(fixture.session.setVolume(0.25F));
    REQUIRE(fixture.session.setVolume(1.0F));

    CHECK((fixture.statePtr->writtenLevels == std::vector<long>{-6000L, -4500L, 0L}));
    CHECK(fixture.statePtr->hardwareElements.front().decibelLevels == std::vector<long>{0L});
    CHECK(fixture.session.stateSnapshot().volume == 1.0F);
  }

  TEST_CASE("AlsaMixerSession - non-finite inputs and huge ranges never enter unsafe rounding",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U},
                        .rawRange = {.min = std::numeric_limits<long>::min(), .max = std::numeric_limits<long>::max()},
                        .rawLevels = {0L}});
    fixture.initialize();

    CHECK(fixture.session.stateSnapshot().volume == 0.5F);
    auto const refreshCount = fixture.statePtr->refreshCount;
    float const renderGain = fixture.session.renderGain();
    fixture.statePtr->refreshSucceeds = false;

    auto const nanRes = fixture.session.setVolume(std::numeric_limits<float>::quiet_NaN());

    REQUIRE_FALSE(nanRes);
    CHECK(nanRes.error().code == Error::Code::InvalidInput);
    CHECK(nanRes.error().message == "ALSA mixer volume request must not be NaN");
    CHECK(fixture.statePtr->refreshCount == refreshCount);
    CHECK(fixture.statePtr->writeCount == 0U);
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::HardwareMixer);
    CHECK(fixture.session.renderGain() == renderGain);

    fixture.statePtr->refreshSucceeds = true;
    REQUIRE(fixture.session.setVolume(-std::numeric_limits<float>::infinity()));
    REQUIRE(fixture.session.setVolume(std::numeric_limits<float>::infinity()));
    REQUIRE(fixture.session.setVolume(0.5F));

    CHECK((fixture.statePtr->writtenLevels ==
           std::vector<long>{std::numeric_limits<long>::min(), std::numeric_limits<long>::max(), 0L}));
  }

  TEST_CASE("AlsaMixerSession - narrow raw and decibel spans retain precision near long limits",
            "[audio][regression][alsa-mixer]")
  {
    for (auto const range : std::array{
           AlsaMixerLevelRange{.min = std::numeric_limits<long>::max() - 2L, .max = std::numeric_limits<long>::max()},
           AlsaMixerLevelRange{.min = std::numeric_limits<long>::min(), .max = std::numeric_limits<long>::min() + 2L}})
    {
      for (bool const decibels : std::array{false, true})
      {
        CAPTURE(range.min, range.max, decibels);
        auto fixture = MixerFixture{};
        auto element =
          FakeMixerElement{.id = {.name = "PCM", .index = 0U}, .rawRange = range, .rawLevels = {range.min + 1L}};

        if (decibels)
        {
          element.optDecibelRange = range;
          element.decibelLevels = {range.min + 1L};
        }

        fixture.addElement(std::move(element));
        fixture.initialize();

        CHECK(fixture.session.stateSnapshot().volume == 0.5F);
        REQUIRE(fixture.session.setVolume(0.0F));
        REQUIRE(fixture.session.setVolume(0.5F));
        CHECK(fixture.session.stateSnapshot().volume == 0.5F);
        REQUIRE(fixture.session.setVolume(1.0F));
        CHECK((fixture.statePtr->writtenLevels == std::vector<long>{range.min, range.min + 1L, range.max}));
      }
    }
  }

  TEST_CASE("AlsaMixerSession - reads refresh external volume and writes relocate an INFO-rebuilt element",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 3U}, .rawLevels = {20L}, .generation = 1U});
    fixture.initialize();
    fixture.statePtr->hardwareElements.front().rawLevels = {80L};

    CHECK(fixture.session.stateSnapshot().volume == 0.8F);

    fixture.statePtr->hardwareElements.front().rawRange = {.min = 0L, .max = 200L};
    fixture.statePtr->hardwareElements.front().rawLevels = {160L};
    fixture.statePtr->hardwareElements.front().generation = 2U;

    REQUIRE(fixture.session.setVolume(0.25F));

    CHECK(fixture.statePtr->writtenLevels == std::vector<long>{50L});
    CHECK(fixture.statePtr->writeGenerations == std::vector<std::size_t>{2U});
    CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{50L});
    CHECK(fixture.statePtr->refreshCount == 3U);
  }

  TEST_CASE("AlsaMixerSession - REMOVE during refresh falls back without using the removed element",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {64L}});
    fixture.initialize();
    fixture.statePtr->hardwareElements.clear();

    auto const volumeRes = fixture.session.setVolume(0.4F);
    auto const state = fixture.session.stateSnapshot();

    REQUIRE_FALSE(volumeRes);
    CHECK(volumeRes.error().code == Error::Code::IoError);
    CHECK(volumeRes.error().message.contains("no volume write was attempted"));
    CHECK(volumeRes.error().message.contains("unity software fallback"));
    CHECK(state.volume == 1.0F);
    CHECK_FALSE(state.applicationMuted);
    CHECK_FALSE(state.effectiveMuted);
    CHECK(state.volumeMode == AlsaVolumeControlMode::SoftwareGain);
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::SoftwareGain);
    CHECK(fixture.session.renderGain() == 1.0F);
    CHECK(fixture.statePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaMixerSession - refresh failure prevents a hardware write and publishes unity fallback",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {70L}});
    fixture.initialize();
    fixture.statePtr->refreshSucceeds = false;

    auto const volumeRes = fixture.session.setVolume(0.4F);

    REQUIRE_FALSE(volumeRes);
    CHECK(volumeRes.error().code == Error::Code::IoError);
    CHECK(volumeRes.error().message.contains("no volume write was attempted"));
    CHECK(volumeRes.error().message.contains("unity software fallback"));
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::SoftwareGain);
    CHECK(fixture.session.stateSnapshot().volume == 1.0F);
    CHECK(fixture.session.renderGain() == 1.0F);
    CHECK(fixture.statePtr->writeCount == 0U);
    CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{70L});
  }

  TEST_CASE("AlsaMixerSession - failed volume write is not compensated after a possible partial change",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {90L, 80L}});
    fixture.initialize();
    fixture.statePtr->writeSucceeds = false;
    fixture.statePtr->applyFirstChannelBeforeWriteFailure = true;

    auto const volumeRes = fixture.session.setVolume(0.25F);

    REQUIRE_FALSE(volumeRes);
    CHECK(volumeRes.error().code == Error::Code::IoError);
    CHECK(volumeRes.error().message.contains("hardware state may have changed partially"));
    CHECK_FALSE(volumeRes.error().message.contains("no volume write was attempted"));
    CHECK(fixture.statePtr->writeCount == 1U);
    CHECK((fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{25L, 80L}));
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::SoftwareGain);
    CHECK(fixture.session.stateSnapshot().volume == 1.0F);
    CHECK(fixture.session.renderGain() == 1.0F);

    REQUIRE(fixture.session.setVolume(0.4F));
    CHECK(fixture.session.stateSnapshot().volume == 0.4F);
    CHECK(fixture.session.renderGain() == 0.4F);
    CHECK(fixture.statePtr->writeCount == 1U);
  }

  TEST_CASE("AlsaMixerSession - failed decibel write reports possible partial hardware effects",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U},
                        .rawLevels = {90L, 80L},
                        .optDecibelRange = AlsaMixerLevelRange{.min = -6000L, .max = 0L},
                        .decibelLevels = {-1200L, -1800L}});
    fixture.initialize();
    fixture.statePtr->writeSucceeds = false;
    fixture.statePtr->applyFirstChannelBeforeWriteFailure = true;

    auto const volumeRes = fixture.session.setVolume(0.25F);

    REQUIRE_FALSE(volumeRes);
    CHECK(volumeRes.error().code == Error::Code::IoError);
    CHECK(volumeRes.error().message.contains("hardware state may have changed partially"));
    CHECK_FALSE(volumeRes.error().message.contains("no volume write was attempted"));
    CHECK(fixture.statePtr->writeCount == 1U);
    CHECK((fixture.statePtr->hardwareElements.front().decibelLevels == std::vector<long>{-4500L, -1800L}));
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::SoftwareGain);
    CHECK(fixture.session.renderGain() == 1.0F);
  }

  TEST_CASE("AlsaMixerSession - external mute changes only effective state and pure application reads do not refresh",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.statePtr->optHardwareMuted = true;
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {88L}});
    fixture.initialize();
    auto const refreshCount = fixture.statePtr->refreshCount;

    CHECK_FALSE(fixture.session.isApplicationMuted());
    CHECK(fixture.statePtr->refreshCount == refreshCount);
    auto const hardwareMutedState = fixture.session.stateSnapshot();
    CHECK_FALSE(hardwareMutedState.applicationMuted);
    CHECK(hardwareMutedState.effectiveMuted);
    CHECK(fixture.session.renderGain() == 1.0F);

    auto const appMutedState = fixture.session.setMuted(true);
    CHECK(appMutedState.applicationMuted);
    CHECK(appMutedState.effectiveMuted);
    CHECK(fixture.session.renderGain() == 0.0F);
    fixture.session.close();
    CHECK(fixture.session.isApplicationMuted());
    CHECK(fixture.session.renderGain() == 0.0F);

    fixture.initialize();
    CHECK(fixture.session.isApplicationMuted());
    CHECK(fixture.session.renderGain() == 0.0F);
    CHECK(fixture.statePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaMixerSession - application mute is software-only and preserves hardware volume mode",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.statePtr->optHardwareMuted = true;
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {88L, 61L}});
    auto const preOpenState = fixture.session.setMuted(true);
    CHECK(preOpenState.applicationMuted);
    CHECK(preOpenState.effectiveMuted);
    CHECK(preOpenState.volumeMode == AlsaVolumeControlMode::Unavailable);
    CHECK(fixture.session.renderGain() == 0.0F);
    fixture.initialize();

    CHECK(fixture.session.renderGain() == 0.0F);
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::HardwareMixer);

    // The native cache still contains 88/61. Application unmute must not invoke
    // a switch setter that could write those stale levels over this external change.
    fixture.statePtr->hardwareElements.front().rawLevels = {99L, 77L};
    auto const stateAfterUnmuteRequest = fixture.session.setMuted(false);

    CHECK(fixture.statePtr->refreshCount == 2U);
    CHECK(fixture.session.renderGain() == 1.0F);
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::HardwareMixer);
    CHECK_FALSE(stateAfterUnmuteRequest.applicationMuted);
    CHECK(stateAfterUnmuteRequest.effectiveMuted);
    REQUIRE(fixture.statePtr->optHardwareMuted);
    CHECK(*fixture.statePtr->optHardwareMuted);
    CHECK(stateAfterUnmuteRequest.volume == 0.99F);
    CHECK(fixture.statePtr->writeCount == 0U);
    CHECK((fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{99L, 77L}));
  }

  TEST_CASE("AlsaMixerSession - mute publication refreshes external switches without writing them",
            "[audio][regression][alsa-mixer]")
  {
    for (bool const initiallyMuted : std::array{false, true})
    {
      CAPTURE(initiallyMuted);
      auto fixture = MixerFixture{};
      fixture.statePtr->optHardwareMuted = initiallyMuted;
      fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
      fixture.initialize();
      fixture.statePtr->optHardwareMuted = !initiallyMuted;
      fixture.statePtr->hardwareElements.front().rawLevels = {62L};

      auto const stateAfterUnmuteRequest = fixture.session.setMuted(false);

      CHECK_FALSE(stateAfterUnmuteRequest.applicationMuted);
      CHECK(stateAfterUnmuteRequest.effectiveMuted == !initiallyMuted);
      CHECK(stateAfterUnmuteRequest.volume == 0.62F);
      CHECK(stateAfterUnmuteRequest.volumeMode == AlsaVolumeControlMode::HardwareMixer);
      CHECK(fixture.session.renderGain() == 1.0F);
      CHECK(fixture.statePtr->refreshCount == 2U);
      auto const mutedState = fixture.session.setMuted(true);
      CHECK(mutedState.applicationMuted);
      CHECK(mutedState.effectiveMuted);
      CHECK(fixture.session.renderGain() == 0.0F);
      auto const unmutedState = fixture.session.setMuted(false);
      CHECK_FALSE(unmutedState.applicationMuted);
      CHECK(unmutedState.effectiveMuted == !initiallyMuted);
      CHECK(fixture.statePtr->writeCount == 0U);
      CHECK(fixture.statePtr->optHardwareMuted == !initiallyMuted);
      CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{62L});
    }
  }

  TEST_CASE("AlsaMixerSession - explicit volume preserves an externally changed switch",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.statePtr->optHardwareMuted = false;
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    fixture.initialize();
    fixture.statePtr->optHardwareMuted = true;

    REQUIRE(fixture.session.setVolume(0.25F));

    CHECK(fixture.statePtr->writeCount == 1U);
    CHECK(fixture.statePtr->optHardwareMuted == true);
    CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{25L});
    CHECK_FALSE(fixture.session.stateSnapshot().applicationMuted);
    CHECK(fixture.session.stateSnapshot().effectiveMuted);
  }

  TEST_CASE("AlsaMixerSession - invalid software volume preserves the existing gain", "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.statePtr->openSucceeds = false;
    CHECK_FALSE(fixture.session.tryInit(nullptr));
    REQUIRE(fixture.session.setVolume(0.4F));

    auto const nanRes = fixture.session.setVolume(std::numeric_limits<float>::quiet_NaN());

    REQUIRE_FALSE(nanRes);
    CHECK(nanRes.error().code == Error::Code::InvalidInput);
    CHECK(fixture.session.stateSnapshot().volume == 0.4F);
    CHECK(fixture.session.renderGain() == 0.4F);
    CHECK(fixture.statePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaMixerSession - render gain follows software controls and mixer lifecycle",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    REQUIRE(fixture.session.setVolume(0.25F));
    CHECK(fixture.session.renderGain() == 1.0F);

    SECTION("mixer open fails")
    {
      fixture.statePtr->openSucceeds = false;
    }

    SECTION("initial refresh fails")
    {
      fixture.statePtr->refreshSucceeds = false;
    }

    SECTION("no readable candidate exists")
    {
    }

    CHECK_FALSE(fixture.session.tryInit(nullptr));
    CHECK(fixture.session.renderGain() == 0.25F);
    CHECK(fixture.session.setMuted(true).applicationMuted);
    REQUIRE(fixture.session.setVolume(0.75F));
    CHECK(fixture.session.renderGain() == 0.0F);
    CHECK_FALSE(fixture.session.setMuted(false).applicationMuted);
    CHECK(fixture.session.renderGain() == 0.75F);

    fixture.statePtr->openSucceeds = true;
    fixture.statePtr->refreshSucceeds = true;
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {64L}});
    fixture.initialize();
    CHECK(fixture.session.renderGain() == 1.0F);
    CHECK(fixture.session.stateSnapshot().volume == 0.64F);

    CHECK(fixture.session.setMuted(true).applicationMuted);
    fixture.session.close();
    CHECK(fixture.session.renderGain() == 0.0F);
    CHECK_FALSE(fixture.session.setMuted(false).applicationMuted);
    CHECK(fixture.session.renderGain() == 1.0F);
    CHECK(fixture.statePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaMixerSession - successful hardware attenuation is not replayed as software gain after failed reopen",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawRange = {.min = 0L, .max = 100L}, .rawLevels = {100L}});
    fixture.initialize();
    REQUIRE(fixture.session.setVolume(0.25F));
    REQUIRE(fixture.session.stateSnapshot().volume == 0.25F);
    REQUIRE(fixture.session.renderGain() == 1.0F);
    fixture.session.close();

    SECTION("mixer open fails")
    {
      fixture.statePtr->openSucceeds = false;
    }

    SECTION("initial refresh fails")
    {
      fixture.statePtr->refreshSucceeds = false;
    }

    SECTION("selected element is no longer readable")
    {
      fixture.statePtr->hardwareElements.front().readable = false;
    }

    CHECK_FALSE(fixture.session.tryInit(nullptr));

    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::SoftwareGain);
    CHECK(fixture.session.stateSnapshot().volume == 1.0F);
    CHECK(fixture.session.renderGain() == 1.0F);
    CHECK(fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{25L});
    CHECK(fixture.statePtr->writtenLevels == std::vector<long>{25L});
    CHECK(fixture.statePtr->writeCount == 1U);
    CHECK(fixture.session.setMuted(true).applicationMuted);
    CHECK(fixture.session.renderGain() == 0.0F);
    CHECK_FALSE(fixture.session.setMuted(false).applicationMuted);
    CHECK(fixture.session.renderGain() == 1.0F);
  }

  TEST_CASE("AlsaMixerSession - hardware write failure preserves mute in the published render gain",
            "[audio][regression][alsa-mixer]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {90L, 80L}});
    fixture.initialize();
    CHECK(fixture.session.setMuted(true).applicationMuted);
    fixture.statePtr->writeSucceeds = false;
    fixture.statePtr->applyFirstChannelBeforeWriteFailure = true;

    CHECK_FALSE(fixture.session.setVolume(0.25F));

    CHECK(fixture.session.renderGain() == 0.0F);
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::SoftwareGain);
    CHECK((fixture.statePtr->hardwareElements.front().rawLevels == std::vector<long>{25L, 80L}));
    REQUIRE(fixture.session.setVolume(0.75F));
    CHECK(fixture.session.renderGain() == 0.0F);
    CHECK_FALSE(fixture.session.setMuted(false).applicationMuted);
    CHECK(fixture.session.renderGain() == 0.75F);
    CHECK(fixture.statePtr->writeCount == 1U);
  }

  TEST_CASE("AlsaMixerSession - mute gain is readable before hardware refresh completes",
            "[audio][regression][alsa-mixer][concurrency]")
  {
    auto fixture = MixerFixture{};
    fixture.addElement({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    fixture.initialize();
    auto enteredPromise = std::promise<void>{};
    auto entered = enteredPromise.get_future().share();
    auto releasePromise = std::promise<void>{};
    auto release = releasePromise.get_future();
    fixture.statePtr->beforeRefresh = [&]
    {
      enteredPromise.set_value();
      release.wait();
    };
    auto mutedPromise = std::promise<AlsaMixerStateSnapshot>{};
    auto muted = mutedPromise.get_future();
    auto gainPromise = std::promise<float>{};
    auto gain = gainPromise.get_future();
    auto render = std::jthread{[&]
                               {
                                 setCurrentThreadName("ao-mixer-gain");
                                 bool const refreshEntered =
                                   entered.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
                                 gainPromise.set_value(refreshEntered ? fixture.session.renderGain() : -1.0F);
                               }};
    auto control = std::jthread{[&]
                                {
                                  setCurrentThreadName("ao-mixer-mute");
                                  mutedPromise.set_value(fixture.session.setMuted(true));
                                }};
    auto const enteredStatus = entered.wait_for(std::chrono::seconds{5});
    auto const gainStatus = gain.wait_for(std::chrono::seconds{5});

    // Release blocked I/O before any assertion, including the timeout guards.
    releasePromise.set_value();
    control.join();
    render.join();
    fixture.statePtr->beforeRefresh = {};

    CHECK(enteredStatus == std::future_status::ready);
    CHECK(gainStatus == std::future_status::ready);
    CHECK(gain.get() == 0.0F);
    auto const mutedSnapshot = muted.get();
    CHECK(mutedSnapshot.applicationMuted);
    CHECK(mutedSnapshot.effectiveMuted);
    CHECK(fixture.session.volumeMode() == AlsaVolumeControlMode::HardwareMixer);
    CHECK(fixture.statePtr->writeCount == 0U);
  }

  TEST_CASE("AlsaMixerSession - concurrent mute and volume changes never expose a muted-only gain",
            "[audio][regression][alsa-mixer][concurrency][stress]")
  {
    auto fixture = MixerFixture{};
    CHECK_FALSE(fixture.session.tryInit(nullptr));
    REQUIRE(fixture.session.setVolume(0.25F));
    auto readerStarted = std::latch{1};
    auto optUnexpectedGain = std::optional<float>{};
    std::size_t sampleCount = 0U;
    auto render = std::jthread{[&](std::stop_token stopToken)
                               {
                                 setCurrentThreadName("ao-mixer-gain");

                                 while (!stopToken.stop_requested())
                                 {
                                   if (float const gain = fixture.session.renderGain(); gain != 0.0F && gain != 0.25F)
                                   {
                                     optUnexpectedGain = gain;
                                   }

                                   ++sampleCount;

                                   if (sampleCount == 1U)
                                   {
                                     readerStarted.count_down();
                                   }
                                 }
                               }};
    readerStarted.wait();
    bool allVolumesApplied = true;

    for (std::size_t iteration = 0; iteration < 10000U; ++iteration)
    {
      std::ignore = fixture.session.setMuted(true);
      bool const raised = fixture.session.setVolume(0.75F).has_value();
      bool const restored = fixture.session.setVolume(0.25F).has_value();
      std::ignore = fixture.session.setMuted(false);
      allVolumesApplied = allVolumesApplied && raised && restored;
    }

    render.request_stop();
    render.join();

    // 0.75 is requested only while muted; no complete control state can render it.
    INFO("Unexpected render gain: " << optUnexpectedGain.value_or(-1.0F));
    CHECK_FALSE(optUnexpectedGain);
    CHECK(sampleCount > 0U);
    CHECK(allVolumesApplied);
    CHECK(fixture.session.renderGain() == 0.25F);
  }
} // namespace ao::audio::backend::detail::test
