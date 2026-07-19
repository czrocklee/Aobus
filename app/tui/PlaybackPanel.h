// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

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
} // namespace ao::rt

namespace ao::uimodel
{
  struct OutputDeviceViewState;
} // namespace ao::uimodel

namespace ao::tui
{
  struct PlaybackBarViewState final
  {
    rt::PlaybackTransportSnapshot const* playbackState = nullptr;
    std::chrono::milliseconds displayElapsed{};
    std::chrono::milliseconds animationElapsed{};
    uimodel::OutputDeviceViewState const* outputView = nullptr;
    ftxui::Box* outputDeviceBox = nullptr;
    ftxui::Box* soulButtonBox = nullptr;
    ftxui::Box* seekRailBox = nullptr;
    bool outputDeviceHovered = false;
    std::int32_t terminalColumns = 0;
  };

  std::int32_t playbackBarRows(std::int32_t terminalRows) noexcept;
  ftxui::Element playbackBar(PlaybackBarViewState const& view);
} // namespace ao::tui
