// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  struct OutputDeviceRowBox final
  {
    std::int32_t rowIndex = -1;
    audio::BackendId backendId{};
    audio::DeviceId deviceId{};
    audio::ProfileId profileId{};
    ftxui::Box box{};
    ftxui::Box secondaryBox{};
  };

  ftxui::Element outputDeviceBadge(uimodel::OutputDeviceViewState const* outputView, bool hovered);
  std::int32_t outputDevicePanelColumns(uimodel::OutputDeviceViewState const& view, std::int32_t terminalColumns);
  ftxui::Element outputDevicePanel(uimodel::OutputDeviceViewState const& view,
                                   std::int32_t selectedRow,
                                   std::vector<OutputDeviceRowBox>* rowBoxes = nullptr,
                                   std::int32_t columns = 0);
} // namespace ao::tui
