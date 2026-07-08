// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListPreview.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  SmartListPreviewStatus deriveSmartListPreviewStatus(bool expressionValid, bool hasPreviewSource)
  {
    if (!expressionValid)
    {
      return SmartListPreviewStatus::InvalidExpression;
    }

    if (!hasPreviewSource)
    {
      return SmartListPreviewStatus::PreviewSourceUnavailable;
    }

    return SmartListPreviewStatus::Valid;
  }

  std::string formatSmartListPreviewStatusText(SmartListPreviewStatus status,
                                               std::size_t count,
                                               bool isAllTracks,
                                               bool localEmpty)
  {
    if (localEmpty)
    {
      if (count == 0)
      {
        return isAllTracks ? "No tracks in library" : "No tracks in source";
      }

      return std::format("Showing all {} {}", count, isAllTracks ? "tracks" : "source tracks");
    }

    switch (status)
    {
      case SmartListPreviewStatus::InvalidExpression: return "Invalid filter";
      case SmartListPreviewStatus::PreviewSourceUnavailable: return "No tracks in source";
      case SmartListPreviewStatus::Valid:
      {
        if (count == 0)
        {
          return "No matches";
        }

        constexpr std::size_t kMaxPreview = 10;

        if (count <= kMaxPreview)
        {
          return std::format("Showing all {} matches", count);
        }

        return std::format("Showing {} of {} matches", kMaxPreview, count);
      }
    }

    return "";
  }

  std::string formatSmartListPreviewTrackLabel(std::string_view title, std::string_view artist, std::string_view album)
  {
    if (!title.empty())
    {
      auto formatted = std::string{title};

      if (!artist.empty())
      {
        formatted = std::format("{} - {}", formatted, artist);
      }

      if (!album.empty())
      {
        formatted = std::format("{} ({})", formatted, album);
      }

      return formatted;
    }

    if (!artist.empty())
    {
      auto formatted = std::string{artist};

      if (!album.empty())
      {
        formatted = std::format("{} ({})", formatted, album);
      }

      return formatted;
    }

    return "(untitled)";
  }
} // namespace ao::uimodel
