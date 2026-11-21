// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OutputDeviceController.h"

#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <utility>

namespace ao::tui
{
  namespace
  {
    using RowKind = uimodel::OutputDeviceRow::Kind;

    bool isSelectable(uimodel::OutputDeviceRow const& row)
    {
      return row.kind == RowKind::DeviceProfile ||
             (row.kind == RowKind::RoutingPolicy && row.optRoutingPolicySelection);
    }

    bool matchesFocus(uimodel::OutputDeviceRow const& previous, uimodel::OutputDeviceRow const& current)
    {
      if (current.kind != previous.kind)
      {
        return false;
      }

      if (previous.kind == RowKind::RoutingPolicy)
      {
        auto const& optPreviousRoute = previous.optRoutingPolicySelection;
        auto const& optCurrentRoute = current.optRoutingPolicySelection;
        // Keep focus when the checkbox changes value, but never move it to another route.
        return optPreviousRoute && optCurrentRoute && optPreviousRoute->backendId == optCurrentRoute->backendId &&
               optPreviousRoute->deviceId == optCurrentRoute->deviceId &&
               optPreviousRoute->profileId == optCurrentRoute->profileId;
      }

      return previous.kind == RowKind::DeviceProfile && previous.backendId == current.backendId &&
             previous.deviceId == current.deviceId && previous.profileId == current.profileId;
    }
  } // namespace

  OutputDeviceController::OutputDeviceController(rt::PlaybackService& playback,
                                                 i18n::MessageCatalog const& textCatalog,
                                                 uimodel::OutputDeviceIntent intent,
                                                 std::function<void()> onChanged)
    : _onChanged{std::move(onChanged)}
    , _viewModel{playback,
                 textCatalog,
                 [this](uimodel::OutputDeviceViewState const& view) { updateView(view); },
                 std::move(intent)}
  {
    refresh();
    selectOpeningRow();
  }

  void OutputDeviceController::prepareToOpen()
  {
    _needsOpeningFocus = true;
    refresh();
  }

  void OutputDeviceController::refresh()
  {
    _viewModel.refresh();
  }

  bool OutputDeviceController::isSelectableRow(std::int32_t const rowIndex) const
  {
    return rowIndex >= 0 && std::cmp_less(rowIndex, _view.rows.size()) &&
           isSelectable(_view.rows[static_cast<std::size_t>(rowIndex)]);
  }

  void OutputDeviceController::updateView(uimodel::OutputDeviceViewState const& view)
  {
    if (isSelectableRow(_selectedRow))
    {
      auto const& previous = _view.rows[static_cast<std::size_t>(_selectedRow)];
      auto const it =
        std::ranges::find_if(view.rows, [&previous](auto const& row) { return matchesFocus(previous, row); });
      _selectedRow = it == view.rows.end() ? -1 : static_cast<std::int32_t>(std::distance(view.rows.begin(), it));
    }

    _view = view;

    if (_needsOpeningFocus)
    {
      selectOpeningRow();
      _needsOpeningFocus = _selectedRow == -1;
    }

    if (_onChanged)
    {
      _onChanged();
    }
  }

  void OutputDeviceController::selectOpeningRow()
  {
    // Status is the pending-output advisory. A pending or named PipeWire route must not put
    // a substitute (or the consent checkbox) under Enter when its own device row is absent.
    bool const requireActive = std::ranges::any_of(
      _view.rows, [](auto const& row) { return row.kind == RowKind::Status || row.kind == RowKind::RoutingPolicy; });
    _selectedRow = preferredDeviceRow(requireActive);
  }

  std::int32_t OutputDeviceController::preferredDeviceRow(bool const activeOnly) const
  {
    std::int32_t firstDeviceRow = -1;

    for (std::size_t index = 0; index < _view.rows.size(); ++index)
    {
      auto const& row = _view.rows[index];

      if (row.kind != RowKind::DeviceProfile)
      {
        continue;
      }

      if (row.isActive)
      {
        return static_cast<std::int32_t>(index);
      }

      if (!activeOnly && firstDeviceRow == -1)
      {
        firstDeviceRow = static_cast<std::int32_t>(index);
      }
    }

    return firstDeviceRow;
  }

  void OutputDeviceController::selectNavigationStart()
  {
    _selectedRow = preferredDeviceRow(false);

    if (_selectedRow == -1)
    {
      auto const it = std::ranges::find_if(_view.rows, isSelectable);
      _selectedRow = it == _view.rows.end() ? -1 : static_cast<std::int32_t>(std::distance(_view.rows.begin(), it));
    }
  }

  bool OutputDeviceController::tryMoveSelection(std::int32_t const delta)
  {
    if (delta == 0)
    {
      return false;
    }

    if (!isSelectableRow(_selectedRow))
    {
      selectNavigationStart();

      if (_selectedRow == -1)
      {
        return false;
      }

      _needsOpeningFocus = false;
      return true;
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

  OutputDeviceActivation OutputDeviceController::activateSelected()
  {
    return activateRow(_selectedRow);
  }

  OutputDeviceActivation OutputDeviceController::activateRow(std::int32_t const rowIndex)
  {
    if (!isSelectableRow(rowIndex))
    {
      return OutputDeviceActivation::None;
    }

    _needsOpeningFocus = false;
    _selectedRow = rowIndex;
    auto const& row = _view.rows[static_cast<std::size_t>(rowIndex)];

    if (row.kind == RowKind::RoutingPolicy)
    {
      if (auto const& optExpected = row.optRoutingPolicySelection; optExpected)
      {
        auto const expected = audio::OutputDeviceSelection{*optExpected};
        return _viewModel.tryToggleOutputRoutingPolicy(expected) ? OutputDeviceActivation::RoutingPolicyChanged
                                                                 : OutputDeviceActivation::None;
      }

      return OutputDeviceActivation::None;
    }

    _viewModel.selectOutputDevice(row.backendId, row.deviceId, row.profileId);
    return OutputDeviceActivation::DeviceSelected;
  }

  void OutputDeviceController::setTextCatalog(i18n::MessageCatalog textCatalog)
  {
    _viewModel.setTextCatalog(std::move(textCatalog));
  }
} // namespace ao::tui
