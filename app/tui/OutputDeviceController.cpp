// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OutputDeviceController.h"

#include <ao/audio/Backend.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace ao::tui
{
  OutputDeviceController::OutputDeviceController(rt::PlaybackService& playback, std::function<void()> onChanged)
    : _onChanged{std::move(onChanged)}
    , _viewModel{playback,
                 [this](uimodel::OutputDeviceViewState const& view)
                 {
                   _view = view;
                   normalizeSelection();

                   if (_onChanged)
                   {
                     _onChanged();
                   }
                 }}
  {
    _outputDevicesChangedSub = playback.onOutputDevicesChanged([this] { refresh(); });
    refresh();
  }

  void OutputDeviceController::refresh()
  {
    _viewModel.refresh();
  }

  bool OutputDeviceController::selectableRow(std::int32_t const rowIndex) const
  {
    return rowIndex >= 0 && std::cmp_less(rowIndex, _view.rows.size()) &&
           _view.rows[static_cast<std::size_t>(rowIndex)].kind == uimodel::OutputDeviceRow::Kind::DeviceProfile;
  }

  void OutputDeviceController::normalizeSelection()
  {
    if (selectableRow(_selectedRow))
    {
      return;
    }

    for (std::size_t index = 0; index < _view.rows.size(); ++index)
    {
      if (_view.rows[index].isActive && _view.rows[index].kind == uimodel::OutputDeviceRow::Kind::DeviceProfile)
      {
        _selectedRow = static_cast<std::int32_t>(index);
        return;
      }
    }

    for (std::size_t index = 0; index < _view.rows.size(); ++index)
    {
      if (_view.rows[index].kind == uimodel::OutputDeviceRow::Kind::DeviceProfile)
      {
        _selectedRow = static_cast<std::int32_t>(index);
        return;
      }
    }

    _selectedRow = -1;
  }

  bool OutputDeviceController::moveSelection(std::int32_t const delta)
  {
    if (_view.rows.empty())
    {
      _selectedRow = -1;
      return false;
    }

    normalizeSelection();

    if (!selectableRow(_selectedRow))
    {
      return false;
    }

    auto const step = delta < 0 ? -1 : 1;
    auto target = _selectedRow + delta;

    while (target >= 0 && std::cmp_less(target, _view.rows.size()))
    {
      if (selectableRow(target))
      {
        _selectedRow = target;
        return true;
      }

      target += step;
    }

    return false;
  }

  std::string OutputDeviceController::selectSelected()
  {
    return selectRow(_selectedRow);
  }

  std::string OutputDeviceController::selectRow(std::int32_t const rowIndex)
  {
    if (!selectableRow(rowIndex))
    {
      return "No output device selected";
    }

    _selectedRow = rowIndex;
    auto const& row = _view.rows[static_cast<std::size_t>(rowIndex)];
    _viewModel.selectOutputDevice(row.backendId, row.deviceId, row.profileId);

    if (row.profileId == audio::kProfileExclusive)
    {
      return std::format("Output: {} (Exclusive)", row.title);
    }

    return std::format("Output: {}", row.title);
  }
} // namespace ao::tui
