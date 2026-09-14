// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <tuple>

namespace ao::uimodel::test
{
  TEST_CASE("SeekInteraction - pointer seek decisions", "[uimodel][unit][playback]")
  {
    constexpr auto kFirstOccurrenceId = rt::PlaybackOccurrenceId{1};
    constexpr auto kSecondOccurrenceId = rt::PlaybackOccurrenceId{2};
    auto model = SeekInteraction{};

    SECTION("disabled, zero-duration, and unidentified sliders ignore interaction")
    {
      model.applyViewState(std::chrono::seconds{12}, false, kFirstOccurrenceId);
      CHECK_FALSE(model.tryBeginPointerInteraction());
      CHECK(model.valueChanged(std::chrono::seconds{5}).action == SeekSliderAction::None);

      model.applyViewState(std::chrono::milliseconds{0}, true, kFirstOccurrenceId);
      CHECK_FALSE(model.tryBeginPointerInteraction());
      CHECK(model.valueChanged(std::chrono::seconds{5}).action == SeekSliderAction::None);

      model.applyViewState(std::chrono::seconds{12}, true, {});
      CHECK_FALSE(model.tryBeginPointerInteraction());
      CHECK(model.valueChanged(std::chrono::seconds{5}).action == SeekSliderAction::None);
    }

    SECTION("pointer changes preview and release commits once")
    {
      model.applyViewState(std::chrono::seconds{20}, true, kFirstOccurrenceId);

      CHECK(model.tryBeginPointerInteraction());
      auto const preview = model.valueChanged(std::chrono::seconds{7});

      CHECK(preview.action == SeekSliderAction::Preview);
      CHECK(preview.occurrenceId == kFirstOccurrenceId);
      CHECK(preview.elapsed == std::chrono::seconds{7});
      CHECK(model.hasPendingFinalSeek());

      auto const commit = model.endPointerInteraction(std::chrono::seconds{8});

      CHECK(commit.action == SeekSliderAction::Commit);
      CHECK(commit.occurrenceId == kFirstOccurrenceId);
      CHECK(commit.elapsed == std::chrono::seconds{8});
      CHECK_FALSE(model.isPointerActive());
      CHECK_FALSE(model.hasPendingFinalSeek());

      CHECK(model.endPointerInteraction(std::chrono::seconds{9}).action == SeekSliderAction::None);
    }

    SECTION("programmatic value changes retain their input occurrence")
    {
      model.applyViewState(std::chrono::seconds{20}, true, kFirstOccurrenceId);

      auto const update = model.valueChanged(std::chrono::seconds{11});
      model.applyViewState(std::chrono::seconds{30}, true, kSecondOccurrenceId);

      CHECK(update.action == SeekSliderAction::Commit);
      CHECK(update.occurrenceId == kFirstOccurrenceId);
      CHECK(update.elapsed == std::chrono::seconds{11});
      CHECK_FALSE(model.hasPendingFinalSeek());
    }

    SECTION("elapsed values are clamped to the active duration")
    {
      model.applyViewState(std::chrono::seconds{20}, true, kFirstOccurrenceId);

      CHECK(model.valueChanged(std::chrono::seconds{30}).elapsed == std::chrono::seconds{20});
      CHECK(model.valueChanged(std::chrono::seconds{-3}).elapsed == std::chrono::milliseconds{0});
    }

    SECTION("replacement leaves the held pointer sequence inert until release")
    {
      model.applyViewState(std::chrono::seconds{20}, true, kFirstOccurrenceId);
      REQUIRE(model.tryBeginPointerInteraction());
      CHECK(model.valueChanged(std::chrono::seconds{5}).action == SeekSliderAction::Preview);

      model.applyViewState(std::chrono::seconds{30}, true, kSecondOccurrenceId);

      CHECK(model.isPointerActive());
      CHECK(model.valueChanged(std::chrono::seconds{15}).action == SeekSliderAction::None);
      CHECK(model.endPointerInteraction(std::chrono::seconds{15}).action == SeekSliderAction::None);
      CHECK_FALSE(model.isPointerActive());

      REQUIRE(model.tryBeginPointerInteraction());
      auto const preview = model.valueChanged(std::chrono::seconds{12});
      auto const commit = model.endPointerInteraction(std::chrono::seconds{13});
      CHECK(preview.occurrenceId == kSecondOccurrenceId);
      CHECK(commit.occurrenceId == kSecondOccurrenceId);
    }

    SECTION("same occurrence updates preserve a held pointer sequence")
    {
      model.applyViewState(std::chrono::seconds{20}, true, kFirstOccurrenceId);
      REQUIRE(model.tryBeginPointerInteraction());
      model.applyViewState(std::chrono::seconds{25}, true, kFirstOccurrenceId);

      CHECK(model.duration() == std::chrono::seconds{20});
      auto const preview = model.valueChanged(std::chrono::seconds{23});
      auto const commit = model.endPointerInteraction(std::chrono::seconds{24});
      CHECK(preview.occurrenceId == kFirstOccurrenceId);
      CHECK(preview.elapsed == std::chrono::seconds{20});
      CHECK(commit.occurrenceId == kFirstOccurrenceId);
      CHECK(commit.elapsed == std::chrono::seconds{20});
    }

    SECTION("reset clears state")
    {
      model.applyViewState(std::chrono::seconds{20}, true, kFirstOccurrenceId);
      CHECK(model.tryBeginPointerInteraction());
      std::ignore = model.valueChanged(std::chrono::seconds{3});

      model.reset();

      CHECK(model.duration() == std::chrono::milliseconds{0});
      CHECK(model.occurrenceId() == rt::PlaybackOccurrenceId{});
      CHECK_FALSE(model.isPointerActive());
      CHECK_FALSE(model.hasPendingFinalSeek());
    }
  }
} // namespace ao::uimodel::test
