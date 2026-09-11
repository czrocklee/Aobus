// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CommandCompletionState.h"
#include "Keymap.h"
#include "ListSearch.h"
#include "PanelWidths.h"
#include "TextFieldModel.h"
#include "TrackDetailLines.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionResult.h>

#include <ftxui/component/event.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui
{
  enum class Overlay : std::uint8_t
  {
    None,
    QualityPanel,
    OutputDevices,
    PresentationPanel,
    Notifications,
    Help,
    GoTo,
    ListChooser,
  };

  enum class WorkspaceFocus : std::uint8_t
  {
    Tracks,
    Lists,
    Detail,
  };

  enum class ShellInputMode : std::uint8_t
  {
    None,
    QuickFilter,
    Command,
  };

  /// Whether a popover blocks interaction with the workspace beneath it.
  bool isModalOverlay(Overlay overlay) noexcept;
  std::string_view overlayLabel(i18n::MessageCatalog const& textCatalog, Overlay overlay);
  std::string overlayHint(i18n::MessageCatalog const& textCatalog, KeymapPlan const& keymapPlan, Overlay overlay);

  class ShellInteractionModel final
  {
  public:
    PanelWidths panelWidths() const noexcept { return _panelWidths; }
    void setPanelWidths(PanelWidths widths) noexcept { _panelWidths = widths; }
    bool isDetailVisible() const noexcept { return _detailVisible; }
    void toggleDetail() noexcept;
    bool isTracksFocused() const noexcept { return _workspaceFocus == WorkspaceFocus::Tracks; }
    bool isDetailFocused() const noexcept { return _workspaceFocus == WorkspaceFocus::Detail; }
    void focusDetail() noexcept;
    DetailSectionState& detailSections() noexcept { return _detailSections; }
    DetailSectionState const& detailSections() const noexcept { return _detailSections; }
    std::int32_t detailScroll() const noexcept { return _detailScroll; }
    void resetDetailScroll() noexcept;
    void scrollDetail(std::int32_t delta, std::int32_t lastRow);
    bool isNavigationPinned() const noexcept { return _navigationPinned; }
    bool isNavigationFocused() const noexcept { return _workspaceFocus == WorkspaceFocus::Lists; }
    void setNavigationPinned(bool pinned) noexcept;
    void focusNavigation() noexcept;
    void focusTracks() noexcept { _workspaceFocus = WorkspaceFocus::Tracks; }
    void reconcileNavigationLayout(bool canDock) noexcept;
    void toggleNavigation(bool canDock) noexcept;
    void toggleNavigationPin() noexcept;
    void switchWorkspaceFocus(bool canDock) noexcept;
    bool isInputActive() const noexcept;
    ShellInputMode inputMode() const noexcept;
    std::string const& inputDraft() const noexcept;
    TextFieldModel const& inputField() const noexcept { return _input; }
    bool tryEditInput(ftxui::Event const& event);
    bool tryMoveInputCursor(std::int32_t column);
    void rememberInput();
    bool tryMoveInputHistory(std::int32_t delta);
    bool isInputTouched() const noexcept;
    std::optional<rt::CompletionResult> const& commandCompletion() const noexcept;
    bool isCompletionNavigated() const noexcept { return _completionNavigated; }
    std::int32_t commandCompletionSelection() const noexcept;
    ListSearch& listSearch() noexcept { return _listSearch; }
    ListSearch const& listSearch() const noexcept { return _listSearch; }
    Overlay overlay() const noexcept;
    std::int32_t overlayScroll() const noexcept { return _overlayScroll; }
    void scrollOverlay(std::int32_t delta, std::int32_t lastRow);

    void beginInput(ShellInputMode mode, std::string draft = {});
    void insertInputText(std::string_view text);
    void backspaceInput();
    void closeInput();
    void setCommandCompletion(std::optional<rt::CompletionResult> optCompletion);
    bool tryMoveCommandCompletion(std::int32_t delta);
    bool tryMoveCommandCompletionByPage(std::int32_t delta);
    bool tryApplyCommandCompletion();
    void clearCommandCompletion();

    void openOverlay(Overlay overlay) noexcept;
    void closeOverlay() noexcept;

  private:
    PanelWidths _panelWidths{};
    bool _detailVisible = false;
    DetailSectionState _detailSections{};
    std::int32_t _detailScroll = 0;
    bool _navigationPinned = true;
    WorkspaceFocus _workspaceFocus = WorkspaceFocus::Tracks;
    ShellInputMode _inputMode = ShellInputMode::None;
    TextFieldModel _input{};
    std::vector<std::string> _commandHistory{};
    std::vector<std::string> _filterHistory{};
    std::optional<std::size_t> _optHistoryIndex{};
    std::string _historyDraft{};
    bool _inputTouched = false;
    CommandCompletionState _completion{};
    bool _completionNavigated = false;
    ListSearch _listSearch{};
    Overlay _overlay = Overlay::None;
    std::int32_t _overlayScroll = 0;
  };
} // namespace ao::tui
