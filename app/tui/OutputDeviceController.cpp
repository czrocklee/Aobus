// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OutputDeviceController.h"

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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

  bool OutputDeviceController::isSelectableRow(std::int32_t const rowIndex) const
  {
    return rowIndex >= 0 && std::cmp_less(rowIndex, _view.rows.size()) &&
           _view.rows[static_cast<std::size_t>(rowIndex)].kind == uimodel::OutputDeviceRow::Kind::DeviceProfile;
  }

  void OutputDeviceController::normalizeSelection()
  {
    if (isSelectableRow(_selectedRow))
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

    if (!isSelectableRow(_selectedRow))
    {
      return false;
    }

    if (delta == 0)
    {
      return false;
    }

    auto const maxRow = static_cast<std::int32_t>(
      std::min<std::size_t>(_view.rows.size() - 1, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())));
    auto const target = static_cast<std::int32_t>(
      std::clamp<std::int64_t>(static_cast<std::int64_t>(_selectedRow) + static_cast<std::int64_t>(delta), 0, maxRow));
    auto const selectTarget = [this](std::int32_t const row)
    {
      if (row != _selectedRow && isSelectableRow(row))
      {
        _selectedRow = row;
        return true;
      }

      return false;
    };

    if (delta > 0)
    {
      for (auto row = target; row <= maxRow; ++row)
      {
        if (selectTarget(row))
        {
          return true;
        }
      }

      for (auto row = target - 1; row > _selectedRow; --row)
      {
        if (selectTarget(row))
        {
          return true;
        }
      }

      return false;
    }

    for (auto row = target; row >= 0; --row)
    {
      if (selectTarget(row))
      {
        return true;
      }
    }

    for (auto row = target + 1; row < _selectedRow; ++row)
    {
      if (selectTarget(row))
      {
        return true;
      }
    }

    return false;
  }

  bool OutputDeviceController::selectSelected()
  {
    return selectRow(_selectedRow);
  }

  bool OutputDeviceController::selectRow(std::int32_t const rowIndex)
  {
    if (!isSelectableRow(rowIndex))
    {
      return false;
    }

    _selectedRow = rowIndex;
    auto const& row = _view.rows[static_cast<std::size_t>(rowIndex)];
    _viewModel.selectOutputDevice(row.backendId, row.deviceId, row.profileId);
    return true;
  }
} // namespace ao::tui
