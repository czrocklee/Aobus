// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackPanel.h"

#include "Model.h"
#include "OutputDevicePanel.h"
#include "SoulButton.h"
#include "Style.h"
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
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

  ftxui::Element playbackBar(PlaybackBarViewState const& view)
  {
    using namespace ftxui;

    auto fallbackState = rt::PlaybackState{};
    auto const& state = view.playbackState == nullptr ? fallbackState : *view.playbackState;
    auto const title = state.nowPlaying.title.empty() ? std::string{"No active track"} : state.nowPlaying.title;
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
    auto const volume = std::format("Vol {}%", static_cast<std::int32_t>(std::round(state.volume.level * 100.0F)));
    auto const soulAura =
      uimodel::resolveSoulAura(state.transport == audio::Transport::Playing, state.ready, state.quality);
    auto outputElementPtr = outputDeviceBadge(view.outputView, view.outputDeviceHovered);
    auto soulButtonElementPtr =
      soulButtonElement(state.transport, uimodel::aobusSoulAuraRgb(soulAura), view.animationElapsed);
    auto seekRailElementPtr = seekRail(effectiveElapsed, state.duration, seekRailColumns(view.terminalColumns));

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

    return hbox({
      std::move(soulButtonElementPtr),
      text(" "),
      text(std::move(titleLine)) | bold | flex,
      text(" "),
      std::move(outputElementPtr),
      text(elapsed),
      text(" "),
      std::move(seekRailElementPtr),
      text(" "),
      text(duration),
      text(" "),
      text(volume),
    });
  }
} // namespace ao::tui
