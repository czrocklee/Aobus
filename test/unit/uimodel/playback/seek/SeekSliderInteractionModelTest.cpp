// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <tuple>

namespace ao::uimodel::test
{
  TEST_CASE("SeekSliderInteractionModel - pointer seek decisions", "[uimodel][unit][playback]")
  {
    auto model = SeekSliderInteractionModel{};

    SECTION("disabled and zero-duration sliders ignore interaction")
    {
      model.applyViewState(std::chrono::seconds{12}, false);
      CHECK_FALSE(model.beginPointerInteraction());
      CHECK(model.valueChanged(std::chrono::seconds{5}).action == SeekSliderAction::None);

      model.applyViewState(std::chrono::milliseconds{0}, true);
      CHECK_FALSE(model.beginPointerInteraction());
      CHECK(model.valueChanged(std::chrono::seconds{5}).action == SeekSliderAction::None);
    }

    SECTION("pointer changes preview and release commits once")
    {
      model.applyViewState(std::chrono::seconds{20}, true);

      CHECK(model.beginPointerInteraction());
      auto preview = model.valueChanged(std::chrono::seconds{7});

      CHECK(preview.action == SeekSliderAction::Preview);
      CHECK(preview.elapsed == std::chrono::seconds{7});
      CHECK(model.hasPendingFinalSeek());

      auto commit = model.endPointerInteraction(std::chrono::seconds{8});

      CHECK(commit.action == SeekSliderAction::Commit);
      CHECK(commit.elapsed == std::chrono::seconds{8});
      CHECK_FALSE(model.isPointerActive());
      CHECK_FALSE(model.hasPendingFinalSeek());

      CHECK(model.endPointerInteraction(std::chrono::seconds{9}).action == SeekSliderAction::None);
    }

    SECTION("programmatic value changes outside pointer interaction commit")
    {
      model.applyViewState(std::chrono::seconds{20}, true);

      auto decision = model.valueChanged(std::chrono::seconds{11});

      CHECK(decision.action == SeekSliderAction::Commit);
      CHECK(decision.elapsed == std::chrono::seconds{11});
      CHECK_FALSE(model.hasPendingFinalSeek());
    }

    SECTION("elapsed values are clamped to the active duration")
    {
      model.applyViewState(std::chrono::seconds{20}, true);

      CHECK(model.valueChanged(std::chrono::seconds{30}).elapsed == std::chrono::seconds{20});
      CHECK(model.valueChanged(std::chrono::seconds{-3}).elapsed == std::chrono::milliseconds{0});
    }

    SECTION("reset clears state")
    {
      model.applyViewState(std::chrono::seconds{20}, true);
      CHECK(model.beginPointerInteraction());
      std::ignore = model.valueChanged(std::chrono::seconds{3});

      model.reset();

      CHECK(model.duration() == std::chrono::milliseconds{0});
      CHECK_FALSE(model.isEnabled());
      CHECK_FALSE(model.isPointerActive());
      CHECK_FALSE(model.hasPendingFinalSeek());
    }
  }
} // namespace ao::uimodel::test
