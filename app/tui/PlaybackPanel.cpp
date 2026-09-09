// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackPanel.h"

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
#include <chrono>
#include <cmath>
#include <cstdint>
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
    auto shufflePtr = text("⇄");
    shufflePtr = succession.shuffle == rt::ShuffleMode::On ? std::move(shufflePtr) | style::accent() | bold
                                                           : std::move(shufflePtr) | dim;
    auto repeatPtr = text(succession.repeat == rt::RepeatMode::One ? "↻1" : "↻ ");
    repeatPtr = succession.repeat == rt::RepeatMode::Off ? std::move(repeatPtr) | dim
                                                         : std::move(repeatPtr) | style::accent() | bold;

    if (view.shuffleBox != nullptr)
    {
      shufflePtr = std::move(shufflePtr) | reflect(*view.shuffleBox);
    }

    if (view.repeatBox != nullptr)
    {
      repeatPtr = std::move(repeatPtr) | reflect(*view.repeatBox);
    }

    auto modesPtr = hbox({std::move(shufflePtr), text(" "), std::move(repeatPtr), text(" ")});
    modesPtr->ComputeRequirement();
    auto const title = playbackTitle(textCatalog, state.nowPlaying);
    auto const artist = state.nowPlaying.artist;
    auto titleLine = title;

    if (!artist.empty())
    {
      titleLine.append(" — ");
      titleLine.append(artist);
    }

    auto const effectiveElapsed = clampedElapsed(view.displayElapsed, state.duration);
    auto const elapsed = formatDuration(effectiveElapsed);
    auto const duration = state.duration.count() > 0 ? formatDuration(state.duration) : std::string{"--:--"};
    auto const volume =
      state.volume.muted
        ? std::string{i18n::requiredText(textCatalog, i18n::MessageId::AudioFindingMuted)}
        : playbackVolume(textCatalog, static_cast<std::int32_t>(std::round(state.volume.level * 100.0F)));
    auto const soulAura = uimodel::resolveSoulAura(state.transport, state.ready, state.quality);
    auto const soulVisual = uimodel::aobusSoulVisualFrame(uimodel::aobusSoulAuraRgb(soulAura), view.soulMotion);
    auto outputElementPtr = outputDeviceBadge(view.outputView, view.outputDeviceHovered);
    auto soulButtonElementPtr = soulButtonElement(state.transport, soulVisual, view.animationElapsed);
    outputElementPtr->ComputeRequirement();
    soulButtonElementPtr->ComputeRequirement();
    auto const fixedColumns = outputElementPtr->requirement().min_x + soulButtonElementPtr->requirement().min_x +
                              cellWidth(elapsed) + cellWidth(duration) + cellWidth(volume) + 5 +
                              modesPtr->requirement().min_x;
    auto const freeColumns = std::max(0, view.terminalColumns - fixedColumns);
    auto const railColumns =
      view.terminalColumns <= 0
        ? seekRailColumns(0)
        : std::min(seekRailColumns(view.terminalColumns), freeColumns - std::min(12, std::max(0, freeColumns - 1)));
    auto seekRailElementPtr = railColumns > 0 ? seekRail(effectiveElapsed, state.duration, railColumns) : text("");

    if (view.terminalColumns > 0)
    {
      titleLine = ellipsizeToCellWidth(titleLine, freeColumns - railColumns);
    }

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

    auto volumePtr = text(volume);

    if (view.volumeBox != nullptr)
    {
      volumePtr = std::move(volumePtr) | ftxui::reflect(*view.volumeBox);
    }

    return hbox({
      std::move(soulButtonElementPtr),
      text(" "),
      text(std::move(titleLine)) | bold | flex,
      text(" "),
      std::move(modesPtr),
      std::move(outputElementPtr),
      text(elapsed),
      text(" "),
      std::move(seekRailElementPtr),
      text(" "),
      text(duration),
      text(" "),
      std::move(volumePtr),
    });
  }
} // namespace ao::tui
