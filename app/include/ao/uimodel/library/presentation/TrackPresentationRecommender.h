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
    Smart,
    Manual,
  };

  struct ListPresentationContext final
  {
    ListId listId = kInvalidListId;
    ListPresentationSourceKind sourceKind = ListPresentationSourceKind::AllTracks;
    std::string_view smartListFilter{};
  };

  /**
   * @brief Recommends a track presentation for a list source.
   *
   * Manual lists preserve their stored order by default. Smart lists use their
   * filter expression to select a useful presentation, while All Tracks uses
   * the normal library fallback.
   *
   * @param context The list kind and smart-list filter, when applicable.
   * @param builtinPresets Available builtin presentation presets.
   * @param customPresets Available custom presentation presets.
   * @return A presentation spec derived from the available presets.
   */
  rt::TrackPresentationSpec recommendPresentation(ListPresentationContext const& context,
                                                  std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                  std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::uimodel
