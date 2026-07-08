// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/list/SmartListTrackPresentationResolver.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace ao::uimodel::test
{
  TEST_CASE("SmartListTrackPresentationResolver - maps known ids after auto option", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    REQUIRE(presets.size() >= 2);

    auto const optId = std::optional<std::string>{std::string{presets[1].spec.id}};

    CHECK(resolveSmartListTrackPresentationIndex(optId, presets) == 2);
    CHECK(resolveSmartListTrackPresentationIndex(std::nullopt, presets) == kSmartListAutoTrackPresentationIndex);
    CHECK(resolveSmartListTrackPresentationIndex(std::optional<std::string>{"unknown"}, presets) ==
          kSmartListAutoTrackPresentationIndex);
  }

  TEST_CASE("SmartListTrackPresentationResolver - resolves builtin manual selection", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    REQUIRE(presets.size() >= 2);

    CHECK(resolveSmartListTrackPresentationId(2, true, "$album = 'Kind of Blue'", presets, {}) == presets[1].spec.id);
  }

  TEST_CASE("SmartListTrackPresentationResolver - uses recommendation for auto and invalid positions",
            "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    auto const autoId = resolveSmartListTrackPresentationId(0, true, "$album = 'Kind of Blue'", presets, {});
    auto const invalidId = resolveSmartListTrackPresentationId(999, false, "$album = 'Kind of Blue'", presets, {});

    CHECK(autoId == "albums");
    CHECK(autoId == invalidId);
  }

  TEST_CASE("SmartListTrackPresentationResolver - falls back for out-of-range manual selection",
            "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();

    CHECK(resolveSmartListTrackPresentationId(presets.size() + 1, true, "$artist = 'Queen'", presets, {}) ==
          rt::kDefaultTrackPresentationId);
  }
} // namespace ao::uimodel::test
