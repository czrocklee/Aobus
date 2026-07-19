// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <cstdint>
#include <functional>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::tui
{
  class OutputDeviceController final
  {
  public:
    OutputDeviceController(rt::PlaybackService& playback, std::function<void()> onChanged = {});

    uimodel::OutputDeviceViewState const& viewState() const noexcept { return _view; }
    std::int32_t selectedRow() const noexcept { return _selectedRow; }

    void refresh();
    bool moveSelection(std::int32_t delta);
    bool selectSelected();
    bool selectRow(std::int32_t rowIndex);

  private:
    bool isSelectableRow(std::int32_t rowIndex) const;
    void normalizeSelection();

    uimodel::OutputDeviceViewState _view{};
    std::int32_t _selectedRow = -1;
    std::function<void()> _onChanged;
    uimodel::OutputDeviceViewModel _viewModel;
  };
} // namespace ao::tui
