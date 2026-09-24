// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/list/SmartListEditing.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel::test
{
  namespace
  {
    constexpr auto kPresetIds = std::array<std::string_view, 13>{"library",
                                                                 "list-order",
                                                                 "songs",
                                                                 "albums",
                                                                 "artists",
                                                                 "performers",
                                                                 "genres",
                                                                 "years",
                                                                 "classical-composers",
                                                                 "classical-conductors",
                                                                 "classical-works",
                                                                 "tagging",
                                                                 "technical"};
  } // namespace

  TEST_CASE("SmartListPresentation - maps known ids after auto option", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    REQUIRE(presets.size() == kPresetIds.size());

    auto const optId = std::optional<std::string>{std::string{presets[1].spec.id}};
    CHECK(presets[1].spec.id == rt::kListOrderTrackPresentationId);
    CHECK(resolveSmartListTrackPresentationIndex(optId, presets) == 2);

    for (std::size_t index = 0; index < kPresetIds.size(); ++index)
    {
      CAPTURE(kPresetIds[index]);
      CHECK(resolveSmartListTrackPresentationIndex(std::optional<std::string>{kPresetIds[index]}, presets) ==
            index + 1);
    }

    CHECK(resolveSmartListTrackPresentationIndex(std::nullopt, presets) == kSmartListAutoTrackPresentationIndex);
    CHECK(resolveSmartListTrackPresentationIndex(std::optional<std::string>{"unknown"}, presets) ==
          kSmartListAutoTrackPresentationIndex);
  }

  TEST_CASE("SmartListPresentation - resolves builtin manual selection", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    REQUIRE(presets.size() == kPresetIds.size());

    CHECK(resolveSmartListTrackPresentationId(2, true, "$album = 'Kind of Blue'", presets, {}) == presets[1].spec.id);
    CHECK(presets[1].spec.id == rt::kListOrderTrackPresentationId);

    for (std::size_t index = 0; index < kPresetIds.size(); ++index)
    {
      CAPTURE(kPresetIds[index]);
      CHECK(resolveSmartListTrackPresentationId(index + 1, true, "$album = 'Kind of Blue'", presets, {}) ==
            kPresetIds[index]);
    }
  }

  TEST_CASE("SmartListPresentation - uses recommendation for auto and invalid positions", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    auto const autoId = resolveSmartListTrackPresentationId(0, true, "$album = 'Kind of Blue'", presets, {});
    auto const invalidId = resolveSmartListTrackPresentationId(999, false, "$album = 'Kind of Blue'", presets, {});

    CHECK(autoId == "albums");
    CHECK(autoId == invalidId);
  }

  TEST_CASE("SmartListPresentation - falls back for out-of-range manual selection", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();

    CHECK(resolveSmartListTrackPresentationId(presets.size() + 1, true, "$artist = 'Queen'", presets, {}) ==
          rt::kDefaultTrackPresentationId);
  }
} // namespace ao::uimodel::test
