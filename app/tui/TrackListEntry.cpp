// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackListEntry.h"

#include "PlaybackStatusFormatter.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/field/TrackRowField.h>

#include <format>
#include <string>
#include <utility>

namespace ao::tui
{
  namespace
  {
    std::string textOrPlaceholder(std::string value)
    {
      return value.empty() ? std::string{kMissingTrackFieldPlaceholder} : std::move(value);
    }
  } // namespace

  std::string trackDisplayTitle(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row)
  {
    if (auto optTitle = uimodel::trackTitleOrFilename(row); optTitle)
    {
      return std::move(*optTitle);
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
    return std::format(
      "{:>2}  {}  {}  {}",
      textOrPlaceholder(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber)),
      trackDisplayTitle(textCatalog, row),
      textOrPlaceholder(row.artist),
      textOrPlaceholder(row.album));
  }
} // namespace ao::tui
