// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Model.h"

#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Transport.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
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

    std::string displayFallback(std::string value)
    {
      return value.empty() ? std::string{"-"} : std::move(value);
    }

    std::size_t depthOf(rt::ListNode const& node, std::vector<rt::ListNode> const& lists)
    {
      std::size_t depth = 0;
      auto parentId = node.parentId;
      std::size_t visited = 0;

      while (parentId != kInvalidListId && visited < lists.size())
      {
        auto const it = std::ranges::find(lists, parentId, &rt::ListNode::id);

        if (it == lists.end())
        {
          break;
        }

        ++depth;
        ++visited;
        parentId = it->parentId;
      }

      return depth;
    }

    struct IndicatorColor final
    {
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
    };

    constexpr auto kMedalColor = IndicatorColor{.red = 0xA8, .green = 0x55, .blue = 0xF7};
    constexpr auto kPositiveColor = IndicatorColor{.red = 0x10, .green = 0xB9, .blue = 0x81};
    constexpr auto kDiagnosticColor = IndicatorColor{.red = 0xF5, .green = 0x9E, .blue = 0x0B};
    constexpr auto kWarningColor = IndicatorColor{.red = 0xF5, .green = 0x9E, .blue = 0x0B};
    constexpr auto kInformationalColor = IndicatorColor{.red = 0x6B, .green = 0x72, .blue = 0x80};
    constexpr auto kClippedColor = IndicatorColor{.red = 0xEF, .green = 0x44, .blue = 0x44};

    void applyIndicatorColor(QualityIndicatorStyle& style, IndicatorColor const color)
    {
      style.red = color.red;
      style.green = color.green;
      style.blue = color.blue;
    }
  } // namespace

  std::string formatDuration(std::chrono::milliseconds const duration)
  {
    auto formatted = uimodel::formatDuration(duration);
    return formatted.empty() ? std::string{"0:00"} : std::move(formatted);
  }

  std::string transportLabel(audio::Transport const transport)
  {
    switch (transport)
    {
      case audio::Transport::Idle: return "Idle";
      case audio::Transport::Opening: return "Opening";
      case audio::Transport::Buffering: return "Buffering";
      case audio::Transport::Playing: return "Playing";
      case audio::Transport::Paused: return "Paused";
      case audio::Transport::Seeking: return "Seeking";
      case audio::Transport::Stopping: return "Stopping";
      case audio::Transport::Error: return "Error";
    }

    return "Unknown";
  }

  bool transportNeedsClockTick(audio::Transport const transport)
  {
    switch (transport)
    {
      case audio::Transport::Opening:
      case audio::Transport::Buffering:
      case audio::Transport::Playing:
      case audio::Transport::Seeking: return true;
      case audio::Transport::Idle:
      case audio::Transport::Paused:
      case audio::Transport::Stopping:
      case audio::Transport::Error: return false;
    }

    return false;
  }

  QualityIndicatorStyle qualityIndicatorStyle(uimodel::AudioQualityCategory const category)
  {
    auto style = QualityIndicatorStyle{};

    switch (category)
    {
      case uimodel::AudioQualityCategory::Medal: applyIndicatorColor(style, kMedalColor); return style;
      case uimodel::AudioQualityCategory::Positive: applyIndicatorColor(style, kPositiveColor); return style;
      case uimodel::AudioQualityCategory::Diagnostic: applyIndicatorColor(style, kDiagnosticColor); return style;
      case uimodel::AudioQualityCategory::Warning: applyIndicatorColor(style, kWarningColor); return style;
      case uimodel::AudioQualityCategory::Informational: applyIndicatorColor(style, kInformationalColor); return style;
      case uimodel::AudioQualityCategory::Clipped: applyIndicatorColor(style, kClippedColor); return style;
      case uimodel::AudioQualityCategory::Unknown:
        applyIndicatorColor(style, kInformationalColor);
        style.label = "Unknown quality";
        return style;
    }

    applyIndicatorColor(style, kInformationalColor);
    style.label = "Unknown quality";
    return style;
  }

  QualityIndicatorStyle qualityIndicatorStyle(audio::Quality const quality)
  {
    auto style = qualityIndicatorStyle(uimodel::audioQualityCategory(quality));
    style.label = uimodel::audioQualityConclusion(quality);

    if (style.label.empty() && quality == audio::Quality::Unknown)
    {
      style.label = "Unknown quality";
    }

    return style;
  }

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

  std::string listNodeIcon(rt::ListNodeKind const kind)
  {
    switch (kind)
    {
      case rt::ListNodeKind::Folder: return "[+]";
      case rt::ListNodeKind::Manual: return "[#]";
      case rt::ListNodeKind::Smart: return "[?]";
    }

    return "[ ]";
  }

  std::string listTitle(ListId const listId, std::vector<LibraryNavItem> const& items)
  {
    auto const it = std::ranges::find(items, listId, &LibraryNavItem::id);
    return it == items.end() ? std::string{"All Tracks"} : it->label;
  }

  std::vector<LibraryNavItem> makeLibraryNavigation(std::vector<rt::ListNode> const& lists)
  {
    auto items = std::vector<LibraryNavItem>{};
    items.reserve(lists.size() + 1);
    items.push_back(LibraryNavItem{
      .id = rt::kAllTracksListId,
      .label = "All Tracks",
      .detail = "library",
      .completionText = "All Tracks",
    });

    auto sorted = lists;
    std::ranges::sort(sorted,
                      [](rt::ListNode const& lhs, rt::ListNode const& rhs)
                      {
                        if (lhs.parentId != rhs.parentId)
                        {
                          return lhs.parentId.raw() < rhs.parentId.raw();
                        }

                        return lhs.name < rhs.name;
                      });

    for (auto const& node : sorted)
    {
      auto const completionText = node.name.empty() ? std::string{"<Unnamed List>"} : node.name;
      auto label = std::string(depthOf(node, sorted) * 2, ' ');
      label.append(listNodeIcon(node.kind));
      label.push_back(' ');
      label.append(completionText);

      items.push_back(LibraryNavItem{
        .id = node.id,
        .label = std::move(label),
        .detail = node.smartExpression.empty() ? std::string{} : std::format("[{}]", node.smartExpression),
        .completionText = completionText,
      });
    }

    return items;
  }

  std::vector<std::string> libraryNavigationLabels(std::vector<LibraryNavItem> const& items)
  {
    auto labels = std::vector<std::string>{};
    labels.reserve(items.size());

    for (auto const& item : items)
    {
      labels.push_back(item.detail.empty() ? item.label : std::format("{} {}", item.label, item.detail));
    }

    return labels;
  }

  std::vector<PresentationNavItem> makePresentationNavigation(
    std::span<rt::TrackPresentationPreset const> const builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> const customPresets)
  {
    auto items = std::vector<PresentationNavItem>{};
    items.reserve(builtinPresets.size() + customPresets.size());

    for (auto const& preset : builtinPresets)
    {
      items.push_back(PresentationNavItem{
        .id = preset.spec.id,
        .label = preset.label.empty() ? preset.spec.id : std::string{preset.label},
        .detail = std::string{preset.description},
      });
    }

    for (auto const& preset : customPresets)
    {
      items.push_back(PresentationNavItem{
        .id = preset.spec.id,
        .label = preset.label.empty() ? preset.spec.id : preset.label,
        .detail =
          preset.basePresetId.empty() ? std::string{"custom"} : std::format("custom from {}", preset.basePresetId),
      });
    }

    return items;
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

  std::string presentationDisplayId(std::string_view const presentationId)
  {
    return presentationId.empty() ? std::string{"default"} : std::string{presentationId};
  }

  std::string presentationBadgeLabel(std::string_view const presentationId)
  {
    return std::format("view:{}", presentationDisplayId(presentationId));
  }

  std::string sectionDisplayName(TrackSection const& section)
  {
    return section.primaryText.empty() ? std::string{"Untitled Section"} : section.primaryText;
  }

  TrackListItem makeTrackListItem(rt::TrackRow const& row)
  {
    auto detail = trackDisplayDetail(row);

    return TrackListItem{.id = row.id,
                         .coverArtId = row.coverArtId,
                         .row = row,
                         .label = trackTableLabel(row),
                         .detail = std::move(detail)};
  }

  std::string trackTableLabel(rt::TrackRow const& row)
  {
    auto trackNo = displayFallback(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber));
    return std::format("{:>2}  {}  {}  {}",
                       trackNo == "-" ? std::string{"--"} : trackNo,
                       trackDisplayTitle(row),
                       row.artist.empty() ? "-" : row.artist,
                       row.album.empty() ? "-" : row.album);
  }

  std::vector<std::string> menuLabels(std::vector<TrackListItem> const& tracks)
  {
    auto labels = std::vector<std::string>{};
    labels.reserve(tracks.size());

    for (auto const& track : tracks)
    {
      labels.push_back(track.label);
    }

    return labels;
  }

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
      {"Track", displayFallback(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber))});
    lines.push_back({"Duration", row.duration.count() > 0 ? formatDuration(row.duration) : std::string{"-"}});
    lines.push_back({"Codec", displayFallback(uimodel::formatCodec(row.codec))});
    lines.push_back({"Sample Rate", displayFallback(uimodel::formatSampleRate(row.sampleRate))});
    lines.push_back({"Bit Depth", displayFallback(uimodel::formatBitDepth(row.bitDepth))});
    lines.push_back({"Tags", blankFallback(row.tags)});
    return lines;
  }

  std::string selectionSummary(std::size_t const trackCount, std::int32_t const selectedIndex)
  {
    if (trackCount == 0)
    {
      return "0 tracks";
    }

    auto const visibleIndex = clampSelection(static_cast<std::size_t>(std::max(0, selectedIndex)), trackCount) + 1;
    return std::format("{} / {} tracks", visibleIndex, trackCount);
  }

  std::int32_t moveSelection(std::int32_t const selectedIndex, std::int32_t const delta, std::size_t const itemCount)
  {
    if (itemCount == 0)
    {
      return 0;
    }

    auto const maxIndex =
      std::min<std::int64_t>(static_cast<std::int64_t>(itemCount - 1), std::numeric_limits<std::int32_t>::max());
    auto const next = static_cast<std::int64_t>(selectedIndex) + static_cast<std::int64_t>(delta);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(next, 0, maxIndex));
  }

  std::size_t clampSelection(std::size_t const selection, std::size_t const itemCount)
  {
    if (itemCount == 0)
    {
      return 0;
    }

    return std::min(selection, itemCount - 1);
  }
} // namespace ao::tui
