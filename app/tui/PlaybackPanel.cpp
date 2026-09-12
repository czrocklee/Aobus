// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackPanel.h"

#include "MouseBindings.h"
#include "OutputDevicePanel.h"
#include "PlaybackStatusFormatter.h"
#include "ShellText.h"
#include "SoulButton.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kMinimumSeekRailColumns = 24;
    constexpr std::int32_t kMaximumSeekRailColumns = 48;
    constexpr std::int32_t kPlaybackRows = 1;
    constexpr std::int32_t kMinimumMetadataFieldColumns = 4;

    std::string playbackTitle(i18n::MessageCatalog const& textCatalog, rt::NowPlayingInfo const& track)
    {
      if (!track.title.empty())
      {
        return track.title;
      }

      if (track.trackId != kInvalidTrackId)
      {
        return i18n::requiredFormat(textCatalog, i18n::MessageId::TrackFallback, {{"id", track.trackId.raw()}});
      }

      return std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiPlaybackNoActiveTrack)};
    }

    std::string repeatGlyph(std::string_view const glyph, std::int32_t const count)
    {
      auto result = std::string{};

      for (std::int32_t index = 0; index < count; ++index)
      {
        result.append(glyph);
      }

      return result;
    }

    ftxui::Element playbackMetadataLabel(std::string value, std::size_t const index, bool const hovered)
    {
      using namespace ftxui;
      auto valuePtr = text(std::move(value));

      if (hovered)
      {
        return std::move(valuePtr) | style::buttonHover();
      }

      if (index == 0)
      {
        return std::move(valuePtr) | style::accent() | bold;
      }

      return std::move(valuePtr) | (index == 2 ? dim : nothing);
    }

    ftxui::Element playbackMetadata(i18n::MessageCatalog const& textCatalog,
                                    rt::NowPlayingInfo const& track,
                                    std::int32_t const columns,
                                    PlaybackMetadataHitRegions* const hitRegions,
                                    std::array<bool, 3> const& hovered)
    {
      using namespace ftxui;

      if (hitRegions != nullptr)
      {
        *hitRegions = {.nowPlaying = track};
      }

      auto const values = std::array{playbackTitle(textCatalog, track), track.artist, track.album};

      auto widths = std::array<std::int32_t, 3>{};
      std::int32_t count = 0;
      std::int32_t total = 0;

      for (std::size_t index = 0; index < values.size(); ++index)
      {
        if (values[index].empty())
        {
          continue;
        }

        // Keep at least four cells per visible field before admitting another.
        if (columns >= 0 && count > 0 && columns < ((count + 1) * kMinimumMetadataFieldColumns) + (count * 3))
        {
          break;
        }

        widths[index] = columns < 0 ? cellWidth(values[index]) : std::min(cellWidth(values[index]), columns);
        total += widths[index] + (count > 0 ? 3 : 0);
        ++count;
      }

      // Preserve the title first, then artist, while retaining recognizable linked fragments.
      for (auto index = widths.size(); index > 0 && columns >= 0 && total > columns; --index)
      {
        auto& width = widths[index - 1];
        auto const minimum = index == 1 ? 0 : std::min(width, kMinimumMetadataFieldColumns);
        auto const reduction = std::min(width - minimum, total - columns);
        width -= reduction;
        total -= reduction;
      }

      auto elements = Elements{};

      for (std::size_t index = 0; index < values.size(); ++index)
      {
        if (widths[index] <= 0)
        {
          continue;
        }

        if (!elements.empty())
        {
          elements.push_back(text(index == 2 && widths[1] > 0 ? " / " : " · ") | dim);
        }

        auto valuePtr = playbackMetadataLabel(ellipsizeToCellWidth(values[index], widths[index]),
                                              index,
                                              track.trackId != kInvalidTrackId && hovered[index]);

        if (track.trackId != kInvalidTrackId)
        {
          if (hitRegions != nullptr)
          {
            auto const boxes = std::array{&hitRegions->title, &hitRegions->artist, &hitRegions->album};
            valuePtr = std::move(valuePtr) | reflect(*boxes[index]);
          }
        }

        elements.push_back(std::move(valuePtr));
      }

      elements.push_back(filler());
      return hbox(std::move(elements));
    }

    std::chrono::milliseconds clampedElapsed(std::chrono::milliseconds const elapsed,
                                             std::chrono::milliseconds const duration)
    {
      if (duration <= std::chrono::milliseconds{0})
      {
        return std::chrono::milliseconds{0};
      }

      return std::clamp(elapsed, std::chrono::milliseconds{0}, duration);
    }

    float seekFraction(std::chrono::milliseconds const elapsed, std::chrono::milliseconds const duration)
    {
      if (duration <= std::chrono::milliseconds{0})
      {
        return 0.0F;
      }

      return std::clamp(
        static_cast<float>(clampedElapsed(elapsed, duration).count()) / static_cast<float>(duration.count()),
        0.0F,
        1.0F);
    }

    std::int32_t seekRailColumns(std::int32_t const terminalColumns) noexcept
    {
      if (terminalColumns <= 0)
      {
        return kMinimumSeekRailColumns;
      }

      return std::clamp(terminalColumns / 3, kMinimumSeekRailColumns, kMaximumSeekRailColumns);
    }

    ftxui::Element seekRail(std::chrono::milliseconds const elapsed,
                            std::chrono::milliseconds const duration,
                            std::int32_t const columns)
    {
      using namespace ftxui;

      if (duration <= std::chrono::milliseconds{0})
      {
        return text(repeatGlyph("─", columns)) | dim | size(WIDTH, EQUAL, columns);
      }

      auto const fraction = seekFraction(elapsed, duration);
      auto const thumbColumn =
        std::clamp(static_cast<std::int32_t>(std::round(fraction * static_cast<float>(columns - 1))), 0, columns - 1);
      auto const inactiveColumns = std::max(0, columns - thumbColumn - 1);

      return hbox({
               text(repeatGlyph("━", thumbColumn)) | style::success(),
               text("●") | style::accent() | bold,
               text(repeatGlyph("─", inactiveColumns)) | dim,
             }) |
             size(WIDTH, EQUAL, columns);
    }
  } // namespace

  std::optional<PlaybackModePreset> playbackModePreset(rt::ShuffleMode const shuffle, rt::RepeatMode const repeat)
  {
    if (shuffle != rt::ShuffleMode::Off && shuffle != rt::ShuffleMode::On)
    {
      return std::nullopt;
    }

    switch (repeat)
    {
      case rt::RepeatMode::Off:
        if (shuffle == rt::ShuffleMode::On)
        {
          return PlaybackModePreset{.code = "SHF-",
                                    .labelSelector = "shuffle",
                                    .next = {.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::All}};
        }

        return PlaybackModePreset{.code = "SEQ-", .labelSelector = "order", .next = {.repeat = rt::RepeatMode::All}};
      case rt::RepeatMode::All:
        if (shuffle == rt::ShuffleMode::On)
        {
          return PlaybackModePreset{
            .code = "SHF*", .labelSelector = "shuffle_all", .next = {.repeat = rt::RepeatMode::One}};
        }

        return PlaybackModePreset{.code = "SEQ*", .labelSelector = "all", .next = {.shuffle = rt::ShuffleMode::On}};
      case rt::RepeatMode::One:
        if (shuffle == rt::ShuffleMode::On)
        {
          return PlaybackModePreset{.code = "SHF1", .labelSelector = "shuffle_one", .next = {}};
        }

        return PlaybackModePreset{.code = "SEQ1", .labelSelector = "one", .next = {}};
    }

    return std::nullopt;
  }

  PlaybackModeChoice nextPlaybackMode(rt::ShuffleMode const shuffle, rt::RepeatMode const repeat)
  {
    auto const optPreset = playbackModePreset(shuffle, repeat);
    return optPreset ? optPreset->next : PlaybackModeChoice{};
  }

  std::int32_t playbackBarRows(std::int32_t const /*terminalRows*/) noexcept
  {
    return kPlaybackRows;
  }

  ftxui::Element playbackBar(i18n::MessageCatalog const& textCatalog, PlaybackBarViewState const& view)
  {
    using namespace ftxui;

    auto fallbackState = rt::PlaybackTransportSnapshot{};
    auto const& state = view.playbackState == nullptr ? fallbackState : *view.playbackState;
    auto const succession = view.succession == nullptr ? rt::PlaybackSuccessionSnapshot{} : *view.succession;
    // One padded target stays stable across modes and languages.
    constexpr std::int32_t kModeButtonColumns = 6;
    auto const optPreset = playbackModePreset(succession.shuffle, succession.repeat);
    auto modePtr = text(optPreset ? std::string{optPreset->code} : std::string{}) | bold | center |
                   size(WIDTH, EQUAL, kModeButtonColumns);
    modePtr = std::move(modePtr) | (optPreset && view.playbackModeHovered ? style::buttonHover() : style::accent());

    if (view.playbackModeBox != nullptr)
    {
      *view.playbackModeBox = kEmptyMouseBox;

      if (optPreset)
      {
        modePtr = std::move(modePtr) | reflect(*view.playbackModeBox);
      }
    }

    modePtr->ComputeRequirement();
    auto const effectiveElapsed = clampedElapsed(view.displayElapsed, state.duration);
    auto const elapsed = formatDuration(effectiveElapsed);
    auto const duration = state.duration.count() > 0 ? formatDuration(state.duration) : std::string{"--:--"};
    constexpr std::int32_t kMaximumVolumePercent = 100;
    auto const mutedVolume = i18n::requiredText(textCatalog, i18n::MessageId::AudioFindingMuted);
    auto const levelVolume =
      playbackVolume(textCatalog, static_cast<std::int32_t>(std::round(state.volume.level * kMaximumVolumePercent)));
    auto const volume = state.volume.muted ? std::string{mutedVolume} : levelVolume;
    // Keep adjacent controls stationary while time ticks or normalized volume changes.
    auto const durationColumns = cellWidth(duration);
    auto const elapsedColumns = std::max(cellWidth(elapsed), state.duration.count() > 0 ? durationColumns : 0);
    auto const minimumVolumeColumns =
      std::max(cellWidth(levelVolume), cellWidth(playbackVolume(textCatalog, kMaximumVolumePercent)));
    auto volumeColumns = std::max(minimumVolumeColumns, cellWidth(mutedVolume));
    auto const soulAura = uimodel::resolveSoulAura(state.transport, state.ready, state.quality);
    auto const soulVisual = uimodel::aobusSoulVisualFrame(uimodel::aobusSoulAuraRgb(soulAura), view.soulMotion);
    auto outputElementPtr = outputDeviceBadge(view.outputView, view.outputDeviceHovered);
    auto soulButtonElementPtr = soulButtonElement(state.transport, soulVisual, view.animationElapsed);
    outputElementPtr->ComputeRequirement();
    soulButtonElementPtr->ComputeRequirement();
    auto const fixedColumnsWithoutVolume = outputElementPtr->requirement().min_x +
                                           soulButtonElementPtr->requirement().min_x + elapsedColumns +
                                           durationColumns + 5 + modePtr->requirement().min_x;
    // Keep readable metadata fragments before assigning the remaining space to the seek rail.
    constexpr std::int32_t kTitleReadabilityColumns = 12;
    constexpr std::int32_t kLinkedFieldReadabilityColumns = 9;

    if (view.terminalColumns > 0)
    {
      // Preserve the numeric volume and title before reserving a longer mute label.
      volumeColumns = std::clamp(view.terminalColumns - fixedColumnsWithoutVolume - kTitleReadabilityColumns - 1,
                                 minimumVolumeColumns,
                                 volumeColumns);
    }

    auto const freeColumns = std::max(0, view.terminalColumns - fixedColumnsWithoutVolume - volumeColumns);
    auto const metadataColumns = kTitleReadabilityColumns +
                                 (state.nowPlaying.artist.empty() ? 0 : kLinkedFieldReadabilityColumns) +
                                 (state.nowPlaying.album.empty() ? 0 : kLinkedFieldReadabilityColumns);
    auto const railColumns = view.terminalColumns <= 0
                               ? seekRailColumns(0)
                               : std::min(seekRailColumns(view.terminalColumns),
                                          freeColumns - std::min(metadataColumns, std::max(0, freeColumns - 1)));
    auto seekRailElementPtr = railColumns > 0 ? seekRail(effectiveElapsed, state.duration, railColumns) : text("");

    auto metadataPtr = playbackMetadata(textCatalog,
                                        state.nowPlaying,
                                        view.terminalColumns > 0 ? freeColumns - railColumns : -1,
                                        view.metadataHitRegions,
                                        {view.titleHovered, view.artistHovered, view.albumHovered});

    if (view.outputDeviceBox != nullptr)
    {
      outputElementPtr = std::move(outputElementPtr) | reflect(*view.outputDeviceBox);
    }

    if (view.soulButtonBox != nullptr)
    {
      soulButtonElementPtr = std::move(soulButtonElementPtr) | reflect(*view.soulButtonBox);
    }

    if (view.seekRailBox != nullptr)
    {
      seekRailElementPtr = std::move(seekRailElementPtr) | reflect(*view.seekRailBox);
    }

    auto volumePtr =
      hbox({filler(), text(ellipsizeToCellWidth(volume, volumeColumns))}) | size(WIDTH, EQUAL, volumeColumns);

    if (view.volumeBox != nullptr)
    {
      volumePtr = std::move(volumePtr) | reflect(*view.volumeBox);
    }

    return hbox({
      std::move(soulButtonElementPtr),
      text(" "),
      std::move(metadataPtr) | flex,
      text(" "),
      std::move(modePtr),
      std::move(outputElementPtr),
      text(elapsed) | size(WIDTH, EQUAL, elapsedColumns),
      text(" "),
      std::move(seekRailElementPtr),
      text(" "),
      text(duration),
      text(" "),
      std::move(volumePtr),
    });
  }
} // namespace ao::tui
