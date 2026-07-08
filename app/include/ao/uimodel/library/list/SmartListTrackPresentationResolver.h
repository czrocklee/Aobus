// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackPresentation.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  inline constexpr std::size_t kSmartListAutoTrackPresentationIndex = 0;

  std::size_t resolveSmartListTrackPresentationIndex(std::optional<std::string> const& optPresentationId,
                                                     std::span<rt::TrackPresentationPreset const> builtinPresets);

  std::string resolveSmartListTrackPresentationId(std::size_t selectedIndex,
                                                  bool selectedIndexValid,
                                                  std::string_view localExpression,
                                                  std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                  std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::uimodel
