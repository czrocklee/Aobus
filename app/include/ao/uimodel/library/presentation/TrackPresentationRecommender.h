// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace ao::uimodel
{
  enum class ListPresentationSourceKind : std::uint8_t
  {
    AllTracks,
    SavedList,
  };

  struct ListPresentationContext final
  {
    ListId listId = kInvalidListId;
    ListPresentationSourceKind sourceKind = ListPresentationSourceKind::AllTracks;
    std::string_view listExpression{};
  };

  /**
   * @brief Recommends a track presentation for a list source.
   *
   * Saved Lists use their local expression to select a useful presentation,
   * while All Tracks uses the normal library fallback.
   *
   * @param context The source kind and saved-List expression, when applicable.
   * @param builtinPresets Available builtin presentation presets.
   * @param customPresets Available custom presentation presets.
   * @return A presentation spec derived from the available presets.
   */
  rt::TrackPresentationSpec recommendPresentation(ListPresentationContext const& context,
                                                  std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                  std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::uimodel
