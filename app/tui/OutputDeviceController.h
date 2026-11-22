// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <cstdint>
#include <functional>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::tui
{
  enum class OutputDeviceActivation : std::uint8_t
  {
    None,
    DeviceSelected,
    RoutingPolicyChanged,
  };

  class OutputDeviceController final
  {
  public:
    /// @param intent Where a chosen route is recorded; `OutputDeviceIntent::discarded()` keeps none.
    OutputDeviceController(rt::PlaybackService& playback,
                           i18n::MessageCatalog const& textCatalog,
                           uimodel::OutputDeviceIntent intent,
                           std::function<void()> onChanged = {});

    uimodel::OutputDeviceViewState const& viewState() const noexcept { return _view; }
    std::int32_t selectedRow() const noexcept { return _selectedRow; }

    /// Prefers the confirmed device; a pending request stays unfocused until the user navigates or it is confirmed.
    void prepareToOpen();
    void refresh();
    bool tryMoveSelection(std::int32_t delta);
    OutputDeviceActivation activateSelected();
    OutputDeviceActivation activateRow(std::int32_t rowIndex);

    void setTextCatalog(i18n::MessageCatalog textCatalog);

  private:
    bool isSelectableRow(std::int32_t rowIndex) const;
    void updateView(uimodel::OutputDeviceViewState const& view);
    void selectOpeningRow();
    std::int32_t preferredDeviceRow(bool activeOnly) const;
    void selectNavigationStart();

    uimodel::OutputDeviceViewState _view{};
    std::int32_t _selectedRow = -1;
    // Only opening can rearm initial focus; losing a focused row must not.
    bool _needsOpeningFocus = false;
    std::function<void()> _onChanged;
    uimodel::OutputDeviceViewModel _viewModel;
  };
} // namespace ao::tui
