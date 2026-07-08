// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackListEntry.h"

#include "PlaybackStatusFormatter.h"
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::string textOrPlaceholder(std::string value)
    {
      return value.empty() ? std::string{"-"} : std::move(value);
    }
  } // namespace

  std::string trackDisplayTitle(rt::TrackRow const& row)
  {
    if (!row.title.empty())
    {
      return row.title;
    }

    if (row.optUriPath)
    {
      return row.optUriPath->filename().string();
    }

    return std::format("Track {}", row.id.raw());
  }

  std::string trackDisplayDetail(rt::TrackRow const& row)
  {
    auto detail = std::string{};

    if (!row.artist.empty())
    {
      detail.append(row.artist);
    }

    if (!row.album.empty())
    {
      if (!detail.empty())
      {
        detail.append(" - ");
      }

      detail.append(row.album);
    }

    if (row.duration.count() > 0)
    {
      if (!detail.empty())
      {
        detail.append("  ");
      }

      detail.append(formatDuration(row.duration));
    }

    return detail;
  }

  TrackListEntry makeTrackListEntry(rt::TrackRow const& row)
  {
    auto detail = trackDisplayDetail(row);

    return TrackListEntry{.id = row.id,
                          .coverArtId = row.coverArtId,
                          .row = row,
                          .label = trackTableLabel(row),
                          .detail = std::move(detail)};
  }

  std::string trackTableLabel(rt::TrackRow const& row)
  {
    auto trackNo = textOrPlaceholder(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber));
    return std::format("{:>2}  {}  {}  {}",
                       trackNo == "-" ? std::string{"--"} : trackNo,
                       trackDisplayTitle(row),
                       row.artist.empty() ? "-" : row.artist,
                       row.album.empty() ? "-" : row.album);
  }

  std::vector<std::string> menuLabels(std::vector<TrackListEntry> const& tracks)
  {
    auto labels = std::vector<std::string>{};
    labels.reserve(tracks.size());

    for (auto const& track : tracks)
    {
      labels.push_back(track.label);
    }

    return labels;
  }
} // namespace ao::tui
