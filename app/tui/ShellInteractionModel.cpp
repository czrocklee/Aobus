// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellInteractionModel.h"

#include "Keymap.h"
#include "ShellText.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/utility/UnicodeText.h>

#include <ftxui/component/event.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  bool isModalOverlay(Overlay const overlay) noexcept
  {
    switch (overlay)
    {
      case Overlay::None:
      case Overlay::DetailPanel: return false;
      case Overlay::ListChooser:
      case Overlay::QualityPanel:
      case Overlay::OutputDevices:
      case Overlay::PresentationPanel:
      case Overlay::Notifications:
      case Overlay::Help: return true;
    }

    return true;
  }

  std::string_view overlayLabel(i18n::MessageCatalog const& textCatalog, Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayTracks);
      case Overlay::ListChooser: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayLists);
      case Overlay::DetailPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayDetail);
      case Overlay::QualityPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayPipeline);
      case Overlay::OutputDevices: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayOutput);
      case Overlay::PresentationPanel: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayViews);
      case Overlay::Notifications:
        return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayNotifications);
      case Overlay::Help: return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayHelp);
    }

    return i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayTracks);
  }

  std::string_view overlayToggleShortcut(KeymapPlan const& keymapPlan, Overlay const overlay)
  {
    static auto const kSelectionEvents = std::to_array({ftxui::Event::Return});
    static auto const kNotificationEvents = std::to_array({ftxui::Event::Character("x")});

    switch (overlay)
    {
      case Overlay::ListChooser: return keymapPlan.shortcutFor(KeyAction::ToggleListChooser, kSelectionEvents);
      case Overlay::DetailPanel: return keymapPlan.shortcutFor(KeyAction::ToggleDetails);
      case Overlay::QualityPanel: return keymapPlan.shortcutFor(KeyAction::ToggleAudioPipeline);
      case Overlay::OutputDevices: return keymapPlan.shortcutFor(KeyAction::ToggleOutputDevices, kSelectionEvents);
      case Overlay::PresentationPanel: return keymapPlan.shortcutFor(KeyAction::TogglePresentations, kSelectionEvents);
      case Overlay::Notifications: return keymapPlan.shortcutFor(KeyAction::ToggleNotifications, kNotificationEvents);
      case Overlay::None:
      case Overlay::Help: return {};
    }

    return {};
  }

  std::string overlayHint(i18n::MessageCatalog const& textCatalog, KeymapPlan const& keymapPlan, Overlay const overlay)
  {
    switch (overlay)
    {
      case Overlay::None: return {};
      case Overlay::ListChooser:
        return overlayHintText(
          textCatalog, i18n::MessageId::TuiShellHintLists, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::DetailPanel:
        return overlayHintText(
          textCatalog, i18n::MessageId::TuiShellHintDetail, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::QualityPanel:
        return overlayHintText(
          textCatalog, i18n::MessageId::TuiShellHintPipeline, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::OutputDevices:
        return overlayHintText(
          textCatalog, i18n::MessageId::TuiShellHintOutput, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::PresentationPanel:
        return overlayHintText(
          textCatalog, i18n::MessageId::TuiShellHintViews, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::Notifications:
        return overlayHintText(
          textCatalog, i18n::MessageId::TuiShellHintNotifications, overlayToggleShortcut(keymapPlan, overlay));
      case Overlay::Help: return overlayHintText(textCatalog, i18n::MessageId::TuiShellHintHelp, {});
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
    return _inputDraft;
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

  void ShellInteractionModel::beginInput(ShellInputMode const mode, std::string draft)
  {
    _inputMode = mode;
    _inputDraft = std::move(draft);
    _inputTouched = !_inputDraft.empty();
    clearCommandCompletion();
  }

  void ShellInteractionModel::appendInputText(std::string_view const text)
  {
    if (text.empty())
    {
      return;
    }

    _inputDraft.append(text);
    _inputTouched = true;
  }

  void ShellInteractionModel::backspaceInput()
  {
    if (_inputDraft.empty())
    {
      return;
    }

    _inputTouched = true;
    auto const boundaryRes = utility::previousUtf8GraphemeBoundary(_inputDraft, _inputDraft.size());

    if (boundaryRes)
    {
      _inputDraft.resize(*boundaryRes);
      return;
    }

    // Terminal input is expected to be valid UTF-8. Preserve the former
    // code-point fallback if an invalid byte sequence or ICU failure reaches
    // this UI-only boundary so Backspace still makes progress.
    constexpr unsigned int kUtf8ContinuationMask = 0xC0U;
    constexpr unsigned int kUtf8ContinuationTag = 0x80U;

    while (!_inputDraft.empty() &&
           (static_cast<unsigned char>(_inputDraft.back()) & kUtf8ContinuationMask) == kUtf8ContinuationTag)
    {
      _inputDraft.pop_back();
    }

    if (!_inputDraft.empty())
    {
      _inputDraft.pop_back();
    }
  }

  void ShellInteractionModel::closeInput()
  {
    _inputMode = ShellInputMode::None;
    _inputDraft.clear();
    _inputTouched = false;
    clearCommandCompletion();
  }

  void ShellInteractionModel::setCommandCompletion(std::optional<rt::CompletionResult> optCompletion)
  {
    _completion.set(std::move(optCompletion));
  }

  bool ShellInteractionModel::tryMoveCommandCompletion(std::int32_t const delta)
  {
    return _completion.tryMoveSelection(delta);
  }

  bool ShellInteractionModel::tryMoveCommandCompletionByPage(std::int32_t const delta)
  {
    return _completion.tryMoveSelectionByPage(delta);
  }

  bool ShellInteractionModel::tryApplyCommandCompletion()
  {
    if (!_completion.tryApplyTo(_inputDraft))
    {
      return false;
    }

    _inputTouched = true;
    return true;
  }

  void ShellInteractionModel::clearCommandCompletion()
  {
    _completion.clear();
  }

  void ShellInteractionModel::openOverlay(Overlay overlay) noexcept
  {
    _overlay = overlay;
  }

  void ShellInteractionModel::closeOverlay() noexcept
  {
    _overlay = Overlay::None;
  }
} // namespace ao::tui
