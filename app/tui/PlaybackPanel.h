// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <ftxui/screen/box.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::rt
{
  struct PlaybackState;
} // namespace ao::rt

namespace ao::tui
{
  inline constexpr std::int32_t kQualityPanelColumns = 48;
  inline constexpr std::int32_t kOutputDevicePanelColumns = 48;

  struct OutputDeviceRowBox final
  {
    std::int32_t rowIndex = -1;
    ftxui::Box box{};
    ftxui::Box secondaryBox{};
  };

  struct PlaybackBarViewState final
  {
    rt::PlaybackState const* playbackState = nullptr;
    std::string_view listTitle{};
    std::string_view presentationId{};
    std::chrono::milliseconds displayElapsed{};
    uimodel::OutputDeviceViewState const* outputView = nullptr;
    ftxui::Box* outputDeviceBox = nullptr;
    ftxui::Box* libraryBox = nullptr;
    ftxui::Box* qualityBox = nullptr;
    ftxui::Box* presentationBox = nullptr;
  };

  ftxui::Element playbackBar(PlaybackBarViewState const& view);

  ftxui::Element qualityPanel(rt::PlaybackState const& state);
  ftxui::Element outputDevicePanel(uimodel::OutputDeviceViewState const& view,
                                   std::int32_t selectedRow,
                                   std::vector<OutputDeviceRowBox>* rowBoxes = nullptr);
} // namespace ao::tui
