// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailLines.h"

#include "PlaybackStatusFormatter.h"
#include "TrackListEntry.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::string_view kAbsentCoreValue = "-";

    constexpr auto kTrackDetailFields = std::to_array<rt::TrackField>({
      rt::TrackField::Title,
      rt::TrackField::Artist,
      rt::TrackField::Album,
      rt::TrackField::AlbumArtist,
      rt::TrackField::Composer,
      rt::TrackField::Conductor,
      rt::TrackField::Ensemble,
      rt::TrackField::Soloist,
      rt::TrackField::Genre,
      rt::TrackField::Year,
      rt::TrackField::DisplayTrackNumber,
      rt::TrackField::Duration,
      rt::TrackField::Codec,
      rt::TrackField::SampleRate,
      rt::TrackField::BitDepth,
      rt::TrackField::Tags,
    });

    std::string numberText(std::uint32_t const value)
    {
      return value == 0 ? std::string{} : std::format("{}", value);
    }
  } // namespace

  std::span<rt::TrackField const> trackDetailFields()
  {
    return kTrackDetailFields;
  }

  std::vector<TrackDetailLine> trackDetailLines(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row)
  {
    auto lines = std::vector<TrackDetailLine>{};
    lines.reserve(kTrackDetailFields.size());

    auto appendCore = [&](rt::TrackField const field, std::string value)
    {
      lines.push_back({.label = std::string{uimodel::trackFieldLabel(textCatalog, field)},
                       .value = value.empty() ? std::string{kAbsentCoreValue} : std::move(value)});
    };
    auto appendOptional = [&](rt::TrackField const field, std::string value)
    {
      if (value.empty())
      {
        return;
      }

      lines.push_back({.label = std::string{uimodel::trackFieldLabel(textCatalog, field)}, .value = std::move(value)});
    };

    appendCore(rt::TrackField::Title, trackDisplayTitle(textCatalog, row));
    appendCore(rt::TrackField::Artist, row.artist);
    appendCore(rt::TrackField::Album, row.album);
    appendOptional(rt::TrackField::AlbumArtist, row.albumArtist);
    appendOptional(rt::TrackField::Composer, row.composer);
    appendOptional(rt::TrackField::Conductor, row.conductor);
    appendOptional(rt::TrackField::Ensemble, row.ensemble);
    appendOptional(rt::TrackField::Soloist, row.soloist);
    appendOptional(rt::TrackField::Genre, row.genre);
    appendOptional(rt::TrackField::Year, numberText(row.year));
    appendCore(rt::TrackField::DisplayTrackNumber,
               uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber));
    appendCore(rt::TrackField::Duration, row.duration.count() > 0 ? formatDuration(row.duration) : std::string{});
    appendOptional(rt::TrackField::Codec, uimodel::formatCodec(row.codec));
    appendOptional(rt::TrackField::SampleRate, uimodel::formatSampleRate(row.sampleRate));
    appendOptional(rt::TrackField::BitDepth, uimodel::formatBitDepth(row.bitDepth));
    appendOptional(rt::TrackField::Tags, row.tags);
    return lines;
  }
} // namespace ao::tui
