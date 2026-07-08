// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailLines.h"

#include "PlaybackStatusFormatter.h"
#include "TrackListEntry.h"
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::string blankFallback(std::string_view value)
    {
      return value.empty() ? std::string{"-"} : std::string{value};
    }

    std::string numberFallback(std::uint32_t const value)
    {
      return value == 0 ? std::string{"-"} : std::format("{}", value);
    }

    std::string textOrPlaceholder(std::string value)
    {
      return value.empty() ? std::string{"-"} : std::move(value);
    }
  } // namespace

  std::vector<TrackDetailLine> trackDetailLines(rt::TrackRow const& row)
  {
    auto lines = std::vector<TrackDetailLine>{};
    constexpr std::size_t kTrackDetailLineCount = 16;
    lines.reserve(kTrackDetailLineCount);
    lines.push_back({"Title", trackDisplayTitle(row)});
    lines.push_back({"Artist", blankFallback(row.artist)});
    lines.push_back({"Album", blankFallback(row.album)});
    lines.push_back({"Album Artist", blankFallback(row.albumArtist)});
    lines.push_back({"Composer", blankFallback(row.composer)});
    lines.push_back({"Conductor", blankFallback(row.conductor)});
    lines.push_back({"Ensemble", blankFallback(row.ensemble)});
    lines.push_back({"Soloist", blankFallback(row.soloist)});
    lines.push_back({"Genre", blankFallback(row.genre)});
    lines.push_back({"Year", numberFallback(row.year)});
    lines.push_back(
      {"Track", textOrPlaceholder(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber))});
    lines.push_back({"Duration", row.duration.count() > 0 ? formatDuration(row.duration) : std::string{"-"}});
    lines.push_back({"Codec", textOrPlaceholder(uimodel::formatCodec(row.codec))});
    lines.push_back({"Sample Rate", textOrPlaceholder(uimodel::formatSampleRate(row.sampleRate))});
    lines.push_back({"Bit Depth", textOrPlaceholder(uimodel::formatBitDepth(row.bitDepth))});
    lines.push_back({"Tags", blankFallback(row.tags)});
    return lines;
  }
} // namespace ao::tui
