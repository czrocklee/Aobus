// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  enum class SmartListPreviewStatus : std::uint8_t
  {
    PreviewSourceUnavailable,
    Valid,
    InvalidExpression
  };

  struct SmartListPreviewState final
  {
    std::string_view name;
    std::string_view localExpression;
    bool hasPreviewSource = false;
    bool hasError = false;
    std::string_view errorMessage;
    std::size_t matchCount = 0;
    bool isAllTracks = false;
  };

  SmartListPreviewStatus deriveSmartListPreviewStatus(bool expressionValid, bool hasPreviewSource);

  std::string formatSmartListPreviewStatusText(SmartListPreviewStatus status,
                                               std::size_t count,
                                               bool isAllTracks,
                                               bool localEmpty);

  std::string formatSmartListPreviewTrackLabel(std::string_view title, std::string_view artist, std::string_view album);
} // namespace ao::uimodel
