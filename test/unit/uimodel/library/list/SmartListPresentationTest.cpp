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
#include <vector>

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
    CHECK(resolveSmartListTrackPresentationIndex(optId, presets, {}) == 2);

    for (std::size_t index = 0; index < kPresetIds.size(); ++index)
    {
      CAPTURE(kPresetIds[index]);
      CHECK(resolveSmartListTrackPresentationIndex(std::optional<std::string>{kPresetIds[index]}, presets, {}) ==
            index + 1);
    }

    CHECK(resolveSmartListTrackPresentationIndex(std::nullopt, presets, {}) == kSmartListAutoTrackPresentationIndex);
    CHECK(resolveSmartListTrackPresentationIndex(std::optional<std::string>{"unknown"}, presets, {}) ==
          kSmartListAutoTrackPresentationIndex);
  }

  TEST_CASE("SmartListPresentation - resolves builtin manual selection", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    REQUIRE(presets.size() == kPresetIds.size());

    CHECK(resolveSmartListTrackPresentationId(2, true, presets, {}) == presets[1].spec.id);
    CHECK(presets[1].spec.id == rt::kListOrderTrackPresentationId);

    for (std::size_t index = 0; index < kPresetIds.size(); ++index)
    {
      CAPTURE(kPresetIds[index]);
      CHECK(resolveSmartListTrackPresentationId(index + 1, true, presets, {}) == kPresetIds[index]);
    }
  }

  TEST_CASE("SmartListPresentation - Auto and invalid selections stay an absent preference", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();

    // Auto is absence of a preference, not today's concrete recommendation:
    // the recommendation happens when a saved Auto list opens through
    // ListPresentations::presentationForList, so the editor hands back the
    // empty id. A builtin selection is unaffected by the Auto semantics.
    CHECK(resolveSmartListTrackPresentationId(0, true, presets, {}).empty());
    CHECK(resolveSmartListTrackPresentationId(999, false, presets, {}).empty());
    CHECK(resolveSmartListTrackPresentationId(2, true, presets, {}) == presets[1].spec.id);
  }

  TEST_CASE("SmartListPresentation - falls back for out-of-range manual selection", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();

    CHECK(resolveSmartListTrackPresentationId(presets.size() + 1, true, presets, {}) ==
          rt::kDefaultTrackPresentationId);
  }

  TEST_CASE("SmartListPresentation - round-trips custom presentations after the builtins", "[uimodel][unit][list]")
  {
    auto const presets = rt::builtinTrackPresentationPresets();
    REQUIRE(presets.size() == kPresetIds.size());

    auto makeCustom = [](std::string_view label, std::string_view id)
    {
      auto preset = rt::CustomTrackPresentationPreset{};
      preset.label = std::string{label};
      preset.spec.id = std::string{id};
      return preset;
    };

    auto const customs = std::vector{makeCustom("Road focus", "custom-road"), makeCustom("Jazz focus", "custom-jazz")};

    // A custom id selects its own option past the builtins and maps back unchanged.
    auto const firstIndex =
      resolveSmartListTrackPresentationIndex(std::optional<std::string>{"custom-road"}, presets, customs);
    auto const secondIndex =
      resolveSmartListTrackPresentationIndex(std::optional<std::string>{"custom-jazz"}, presets, customs);

    CHECK(firstIndex == presets.size() + 1);
    CHECK(secondIndex == presets.size() + 2);

    CHECK(resolveSmartListTrackPresentationId(firstIndex, true, presets, customs) == "custom-road");
    CHECK(resolveSmartListTrackPresentationId(secondIndex, true, presets, customs) == "custom-jazz");

    // Builtin behavior is unchanged while customs are present.
    CHECK(resolveSmartListTrackPresentationIndex(std::optional<std::string>{"list-order"}, presets, customs) == 2);
    CHECK(resolveSmartListTrackPresentationId(2, true, presets, customs) == "list-order");

    // An unassigned list keeps Auto even when customs exist, and an id unknown
    // to both spans still selects Auto.
    CHECK(resolveSmartListTrackPresentationIndex(std::nullopt, presets, customs) ==
          kSmartListAutoTrackPresentationIndex);
    CHECK(resolveSmartListTrackPresentationIndex(std::optional<std::string>{"custom-missing"}, presets, customs) ==
          kSmartListAutoTrackPresentationIndex);

    // A manual selection past the builtins and the customs still falls back.
    CHECK(resolveSmartListTrackPresentationId(presets.size() + customs.size() + 1, true, presets, customs) ==
          rt::kDefaultTrackPresentationId);
  }
} // namespace ao::uimodel::test
