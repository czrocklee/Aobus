// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellInteractionModel.h"

#include "Keymap.h"
#include "ShellText.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionResult.h>

#include <ftxui/component/event.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  void ShellInteractionModel::setNavigationPinned(bool const pinned) noexcept
  {
    _navigationPinned = pinned;

    if (!pinned && isNavigationFocused())
    {
      focusTracks();
    }
  }

  void ShellInteractionModel::focusNavigation() noexcept
  {
    _workspaceFocus = WorkspaceFocus::Lists;
  }

  void ShellInteractionModel::reconcileNavigationLayout(bool const canDock) noexcept
  {
    if (!_navigationPinned || !canDock)
    {
      if (isNavigationFocused())
      {
        focusTracks();
      }

      return;
    }

    if (_overlay == Overlay::ListChooser)
    {
      closeOverlay();
      focusNavigation();
    }
  }

  void ShellInteractionModel::toggleNavigation(bool const canDock) noexcept
  {
    if (!_navigationPinned || !canDock)
    {
      focusTracks();

      if (_overlay == Overlay::ListChooser)
      {
        closeOverlay();
      }
      else
      {
        openOverlay(Overlay::ListChooser);
      }

      return;
    }

    focusNavigation();
  }

  void ShellInteractionModel::toggleNavigationPin() noexcept
  {
    setNavigationPinned(!_navigationPinned);

    if (_navigationPinned)
    {
      focusNavigation();
    }
  }

  void ShellInteractionModel::switchWorkspaceFocus(bool const canDock) noexcept
  {
    if (isNavigationFocused() || isDetailFocused())
    {
      focusTracks();
    }
    else if (_navigationPinned && canDock)
    {
      focusNavigation();
    }
  }

  bool isModalOverlay(Overlay const overlay) noexcept
  {
    switch (overlay)
    {
      case Overlay::None: return false;
      case Overlay::QualityPanel:
      case Overlay::OutputDevices:
      case Overlay::PresentationPanel:
      case Overlay::Notifications:
      case Overlay::Help:
      case Overlay::ListChooser:
      case Overlay::GoTo: return true;
    }

    return true;
  }

  std::string_view overlayLabel(i18n::MessageCatalog const& textCatalog, Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayTracks);
      case Overlay::QualityPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayPipeline);
      case Overlay::OutputDevices: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayOutput);
      case Overlay::PresentationPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayViews);
      case Overlay::Notifications:
        return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayNotifications);
      case Overlay::Help: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayHelp);
      case Overlay::ListChooser: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayLists);
      case Overlay::GoTo: return i18n::requiredText(textCatalog, i18n::MessageId::TuiGoToTitle);
    }

    return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayTracks);
  }

  std::string overlayHint(i18n::MessageCatalog const& textCatalog, KeymapPlan const& keymapPlan, Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return {};
      case Overlay::ListChooser:
      {
        auto const unavailable = std::to_array({ftxui::Event::Return,
                                                ftxui::Event::Character("j"),
                                                ftxui::Event::Character("k"),
                                                ftxui::Event::Character("/")});
        auto const pinKey = keymapPlan.shortcutFor(KeyAction::TogglePinnedLists, unavailable);
        return i18n::requiredFormat(textCatalog,
                                    i18n::MessageId::TuiShellHintLists,
                                    {{"toggleState", "unbound"},
                                     {"toggleKey", ""},
                                     {"pinState", pinKey.empty() ? "unbound" : "bound"},
                                     {"pinKey", pinKey},
                                     {"openKey", "Enter"},
                                     {"closeKey", "Esc"}});
      }
      case Overlay::QualityPanel: return overlayHintText(textCatalog, i18n::MessageId::TuiShellHintPipeline);
      case Overlay::OutputDevices: return overlayHintText(textCatalog, i18n::MessageId::TuiShellHintOutput);
      case Overlay::PresentationPanel: return overlayHintText(textCatalog, i18n::MessageId::TuiShellHintViews);
      case Overlay::Notifications: return overlayHintText(textCatalog, i18n::MessageId::TuiShellHintNotifications);
      case Overlay::GoTo: return std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiGoToHint)};
      case Overlay::Help: return overlayHintText(textCatalog, i18n::MessageId::TuiShellHintHelp);
    }

    return {};
  }

  bool ShellInteractionModel::isInputActive() const noexcept
  {
    return _inputMode != ShellInputMode::None;
  }

  ShellInputMode ShellInteractionModel::inputMode() const noexcept
  {
    return _inputMode;
  }

  std::string const& ShellInteractionModel::inputDraft() const noexcept
  {
    return _input.value();
  }

  bool ShellInteractionModel::tryEditInput(ftxui::Event const& event)
  {
    if (!_input.tryApplyEvent(event))
    {
      return false;
    }

    _inputTouched = true;
    _optHistoryIndex.reset();
    return true;
  }

  bool ShellInteractionModel::tryMoveInputCursor(std::int32_t const column)
  {
    return _input.tryMoveToCell(column);
  }

  void ShellInteractionModel::rememberInput()
  {
    if (_input.empty())
    {
      return;
    }

    constexpr std::size_t kHistoryLimit = 50;
    auto& entries = _inputMode == ShellInputMode::Command ? _commandHistory : _filterHistory;
    std::erase(entries, _input.value());
    entries.push_back(_input.value());

    if (entries.size() > kHistoryLimit)
    {
      entries.erase(entries.begin());
    }
  }

  bool ShellInteractionModel::tryMoveInputHistory(std::int32_t const delta)
  {
    auto const& entries = _inputMode == ShellInputMode::Command ? _commandHistory : _filterHistory;

    if (entries.empty() || (delta >= 0 && !_optHistoryIndex))
    {
      return false;
    }

    if (!_optHistoryIndex)
    {
      _historyDraft = _input.value();
      _optHistoryIndex = entries.size();
    }

    auto const next = std::clamp(
      static_cast<std::int64_t>(*_optHistoryIndex) + delta, std::int64_t{0}, static_cast<std::int64_t>(entries.size()));

    if (std::cmp_equal(next, *_optHistoryIndex))
    {
      return false;
    }

    _optHistoryIndex = static_cast<std::size_t>(next);
    _input.reset(*_optHistoryIndex == entries.size() ? _historyDraft : entries[*_optHistoryIndex]);
    _inputTouched = true;
    clearCommandCompletion();
    return true;
  }

  bool ShellInteractionModel::isInputTouched() const noexcept
  {
    return _inputTouched;
  }

  std::optional<rt::CompletionResult> const& ShellInteractionModel::commandCompletion() const noexcept
  {
    return _completion.result();
  }

  std::int32_t ShellInteractionModel::commandCompletionSelection() const noexcept
  {
    return _completion.selection();
  }

  Overlay ShellInteractionModel::overlay() const noexcept
  {
    return _overlay;
  }

  void ShellInteractionModel::toggleDetail() noexcept
  {
    _detailVisible = !_detailVisible;

    if (!_detailVisible && isDetailFocused())
    {
      focusTracks();
    }

    resetDetailScroll();
  }

  void ShellInteractionModel::focusDetail() noexcept
  {
    _detailVisible = true;
    _workspaceFocus = WorkspaceFocus::Detail;
    _detailSections.revealSelected = true;
  }

  void ShellInteractionModel::resetDetailScroll() noexcept
  {
    _detailScroll = 0;
    _detailSections.revealSelected = false;
  }

  void ShellInteractionModel::scrollDetail(std::int32_t const delta, std::int32_t const lastRow)
  {
    _detailScroll = static_cast<std::int32_t>(
      std::clamp<std::int64_t>(static_cast<std::int64_t>(_detailScroll) + delta, 0, std::max(0, lastRow)));
  }

  void ShellInteractionModel::scrollOverlay(std::int32_t const delta, std::int32_t const lastRow)
  {
    _overlayScroll = static_cast<std::int32_t>(
      std::clamp<std::int64_t>(static_cast<std::int64_t>(_overlayScroll) + delta, 0, std::max(0, lastRow)));
  }

  void ShellInteractionModel::beginInput(ShellInputMode const mode, std::string draft)
  {
    if (_overlay == Overlay::GoTo)
    {
      closeOverlay();
    }

    _inputMode = mode;
    _input.reset(std::move(draft));
    _optHistoryIndex.reset();
    _historyDraft.clear();
    _inputTouched = !_input.empty();
    clearCommandCompletion();
  }

  void ShellInteractionModel::insertInputText(std::string_view const text)
  {
    if (_input.tryInsert(text))
    {
      _inputTouched = true;
      _optHistoryIndex.reset();
    }
  }

  void ShellInteractionModel::backspaceInput()
  {
    if (_input.tryBackspace())
    {
      _inputTouched = true;
      _optHistoryIndex.reset();
    }
  }

  void ShellInteractionModel::closeInput()
  {
    _inputMode = ShellInputMode::None;
    _input.reset("");
    _inputTouched = false;
    clearCommandCompletion();
  }

  void ShellInteractionModel::setCommandCompletion(std::optional<rt::CompletionResult> optCompletion)
  {
    _completionNavigated = false;
    _completion.set(std::move(optCompletion));
  }

  bool ShellInteractionModel::tryMoveCommandCompletion(std::int32_t const delta)
  {
    _completionNavigated = true;
    return _completion.tryMoveSelection(delta);
  }

  bool ShellInteractionModel::tryMoveCommandCompletionByPage(std::int32_t const delta)
  {
    _completionNavigated = true;
    return _completion.tryMoveSelectionByPage(delta);
  }

  bool ShellInteractionModel::tryApplyCommandCompletion()
  {
    if (!_completion.tryApplyTo(_input))
    {
      return false;
    }

    _inputTouched = true;
    _optHistoryIndex.reset();
    clearCommandCompletion();
    return true;
  }

  void ShellInteractionModel::clearCommandCompletion()
  {
    _completionNavigated = false;
    _completion.clear();
  }

  void ShellInteractionModel::openOverlay(Overlay overlay) noexcept
  {
    _listSearch.clear();
    _overlay = overlay;
    _overlayScroll = 0;
  }

  void ShellInteractionModel::closeOverlay() noexcept
  {
    _listSearch.clear();
    _overlay = Overlay::None;
  }
} // namespace ao::tui
