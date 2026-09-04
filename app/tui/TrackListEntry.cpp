// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackListEntry.h"

#include "PlaybackStatusFormatter.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/utility/Path.h>

#include <format>
#include <string>
#include <utility>

namespace ao::tui
{
  namespace
  {
    std::string textOrPlaceholder(std::string value)
    {
      return value.empty() ? std::string{"-"} : std::move(value);
    }
  } // namespace

  std::string trackDisplayTitle(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row)
  {
    if (!row.title.empty())
    {
      return row.title;
    }

    if (row.optUriPath)
    {
      return utility::pathToUtf8(row.optUriPath->filename());
    }

    return i18n::requiredFormat(textCatalog, i18n::MessageId::TrackFallback, {{"id", row.id.raw()}});
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

  TrackListEntry makeTrackListEntry(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row)
  {
    auto detail = trackDisplayDetail(row);

    return TrackListEntry{.id = row.id,
                          .coverArtId = row.coverArtId,
                          .row = row,
                          .label = trackTableLabel(textCatalog, row),
                          .detail = std::move(detail)};
  }

  std::string trackTableLabel(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row)
  {
    auto trackNo = textOrPlaceholder(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber));
    return std::format("{:>2}  {}  {}  {}",
                       trackNo == "-" ? std::string{"--"} : trackNo,
                       trackDisplayTitle(textCatalog, row),
                       row.artist.empty() ? "-" : row.artist,
                       row.album.empty() ? "-" : row.album);
  }
} // namespace ao::tui
