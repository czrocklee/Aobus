// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <ftxui/screen/box.hpp>

#include <chrono>
#include <cstdint>
#include <memory>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::rt
{
  struct PlaybackTransportSnapshot;
  struct PlaybackSuccessionSnapshot;
} // namespace ao::rt

namespace ao::uimodel
{
  struct OutputDeviceViewState;
} // namespace ao::uimodel

namespace ao::tui
{
  struct PlaybackMetadataHitRegions final
  {
    rt::NowPlayingInfo nowPlaying{};
    ftxui::Box title = {.x_max = -1, .y_max = -1};
    ftxui::Box artist = {.x_max = -1, .y_max = -1};
    ftxui::Box album = {.x_max = -1, .y_max = -1};
  };

  struct PlaybackBarViewState final
  {
    rt::PlaybackTransportSnapshot const* playbackState = nullptr;
    rt::PlaybackSuccessionSnapshot const* succession = nullptr;
    std::chrono::milliseconds displayElapsed{};
    std::chrono::milliseconds animationElapsed{};
    uimodel::AobusSoulMotionFrame soulMotion{};
    uimodel::OutputDeviceViewState const* outputView = nullptr;
    ftxui::Box* outputDeviceBox = nullptr;
    ftxui::Box* soulButtonBox = nullptr;
    ftxui::Box* seekRailBox = nullptr;
    ftxui::Box* volumeBox = nullptr;
    ftxui::Box* shuffleBox = nullptr;
    ftxui::Box* repeatBox = nullptr;
    PlaybackMetadataHitRegions* metadataHitRegions = nullptr;
    bool outputDeviceHovered = false;
    std::int32_t terminalColumns = 0;
  };

  std::int32_t playbackBarRows(std::int32_t terminalRows) noexcept;
  ftxui::Element playbackBar(i18n::MessageCatalog const& textCatalog, PlaybackBarViewState const& view);
} // namespace ao::tui
