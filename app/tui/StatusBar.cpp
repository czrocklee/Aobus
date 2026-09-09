// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusBar.h"

#include "CommandPalettePanel.h"
#include "Keymap.h"
#include "MouseBindings.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include "TextField.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kExpandedWorkspaceHintColumns = 100;

    ftxui::Element statusAction(ftxui::Element elementPtr, KeyAction action, StatusBarViewState const& state)
    {
      if (state.actionHitRegions != nullptr)
      {
        state.actionHitRegions->push_back(StatusActionHitRegion{.action = action});
        elementPtr = std::move(elementPtr) | ftxui::reflect(state.actionHitRegions->back().box);
      }

      return elementPtr;
    }

    ftxui::Element workspaceShortcuts(i18n::MessageCatalog const& textCatalog,
                                      StatusBarViewState const& state,
                                      KeymapPlan const& keymapPlan,
                                      std::int32_t availableColumns)
    {
      using namespace ftxui;
      auto parts = Elements{};

      auto appendSeparator = [&]
      {
        if (!parts.empty())
        {
          parts.push_back(style::mutedSeparator());
        }
      };

      auto appendChip = [&](KeyAction action, std::string_view const key, std::string_view const label)
      {
        if (key.empty())
        {
          return;
        }

        auto const columns = cellWidth(key) + 1 + cellWidth(label) + (parts.empty() ? 0 : 3);

        if (columns > availableColumns)
        {
          return;
        }

        availableColumns -= columns;
        appendSeparator();
        parts.push_back(statusAction(style::shortcutChip(key, label), action, state));
      };

      auto appendActionChip = [&](KeyAction const action, std::string_view const label)
      { appendChip(action, keymapPlan.shortcutFor(action), label); };

      auto const filterLabel = i18n::requiredText(textCatalog, i18n::MessageId::TuiShellFilterLabel);
      auto const filterShortcut = keymapPlan.shortcutFor(KeyAction::OpenQuickFilter);

      if (state.filterDraft.empty())
      {
        appendActionChip(KeyAction::PlaySelection, i18n::requiredText(textCatalog, i18n::MessageId::TuiStatusPlay));
        appendChip(KeyAction::OpenQuickFilter, filterShortcut, filterLabel);
      }
      else
      {
        auto const clearKey = keymapPlan.shortcutFor(KeyAction::ClearFilter);
        auto const clearLabel = i18n::requiredText(textCatalog,
                                                   state.terminalColumns < kExpandedWorkspaceHintColumns
                                                     ? i18n::MessageId::TuiStatusClear
                                                     : i18n::MessageId::TuiShellStatusClearFilter);
        auto const clearColumns = clearKey.empty() ? 0 : cellWidth(clearKey) + 1 + cellWidth(clearLabel) + 3;
        auto const filterKey = filterShortcut.empty() ? filterLabel : filterShortcut;
        auto const valueColumns = std::max(1, availableColumns - clearColumns - cellWidth(filterKey) - 1);
        auto const value = state.filterInvalid ? "! " + state.filterDraft : state.filterDraft;
        appendChip(KeyAction::OpenQuickFilter, filterKey, ellipsizeToCellWidth(value, valueColumns));
        appendChip(KeyAction::ClearFilter, clearKey, clearLabel);
      }

      appendActionChip(
        KeyAction::OpenCommandPalette, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusCommand));

      if (state.terminalColumns >= kExpandedWorkspaceHintColumns)
      {
        appendActionChip(KeyAction::ToggleLists, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusLists));
        appendActionChip(
          KeyAction::TogglePresentations, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusView));
        appendActionChip(
          KeyAction::ToggleDetails, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusDetail));
      }

      return hbox(std::move(parts));
    }

    ftxui::Element workspaceEntryPoints(i18n::MessageCatalog const& textCatalog,
                                        StatusBarViewState const& state,
                                        KeymapPlan const& keymapPlan)
    {
      using namespace ftxui;

      auto settingsPtr = style::shortcutChip(keymapPlan.shortcutFor(KeyAction::OpenSettings),
                                             i18n::requiredText(textCatalog, i18n::MessageId::TuiSettingsTitle));

      if (state.settingsHovered)
      {
        settingsPtr = std::move(settingsPtr) | style::buttonHover();
      }

      if (state.settingsButtonBox != nullptr)
      {
        settingsPtr = std::move(settingsPtr) | reflect(*state.settingsButtonBox);
      }

      auto entryPoints = Elements{std::move(settingsPtr)};
      auto const helpShortcut = keymapPlan.shortcutFor(KeyAction::ShowHelp);

      if (!helpShortcut.empty())
      {
        entryPoints.push_back(style::mutedSeparator());
        entryPoints.push_back(statusAction(
          style::shortcutChip(helpShortcut, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusHelp)),
          KeyAction::ShowHelp,
          state));
      }

      return hbox(std::move(entryPoints));
    }
    ftxui::Element visualSelectionStatus(i18n::MessageCatalog const& textCatalog,
                                         StatusBarViewState const& state,
                                         KeymapPlan const& keymapPlan)
    {
      using namespace ftxui;

      if (state.activityStatusBox != nullptr)
      {
        *state.activityStatusBox = kEmptyMouseBox;
      }

      auto parts = Elements{text(std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiLibraryVisualMode)}) |
                            style::accent() | bold};
      auto const keepKey = keymapPlan.shortcutFor(KeyAction::SelectVisual);

      if (!keepKey.empty())
      {
        parts.push_back(style::mutedSeparator());
        parts.push_back(statusAction(
          style::shortcutChip(keepKey, i18n::requiredText(textCatalog, i18n::MessageId::TuiStatusKeepMarks)),
          KeyAction::SelectVisual,
          state));
      }

      parts.push_back(style::mutedSeparator());
      auto cancelPtr =
        style::shortcutChip("Esc", i18n::requiredText(textCatalog, i18n::MessageId::TuiStatusCancelRange));

      if (state.cancelSelectionBox != nullptr)
      {
        cancelPtr = std::move(cancelPtr) | reflect(*state.cancelSelectionBox);
      }

      parts.push_back(std::move(cancelPtr));
      parts.push_back(filler());
      return hbox(std::move(parts));
    }

    ftxui::Element activityStatusSlot(StatusBarViewState const& state,
                                      std::int32_t const minColumns = style::kClassicStatusSlotColumns)
    {
      using namespace ftxui;

      auto bodyPtr = activityCompactLine(state.activityStatus->compact, state.activityStatusHovered);

      if (state.activityStatusHovered)
      {
        bodyPtr = std::move(bodyPtr) | style::buttonHover();
      }

      auto slotPtr = style::statusSlot(std::move(bodyPtr), minColumns);

      if (state.activityStatusBox != nullptr)
      {
        slotPtr = std::move(slotPtr) | reflect(*state.activityStatusBox);
      }

      return slotPtr;
    }

    ftxui::Element workspaceStatus(i18n::MessageCatalog const& textCatalog,
                                   StatusBarViewState const& state,
                                   KeymapPlan const& keymapPlan)
    {
      using namespace ftxui;
      constexpr std::int32_t kMaximumActivityColumns = 36;
      auto entryPointsPtr = workspaceEntryPoints(textCatalog, state, keymapPlan);
      entryPointsPtr->ComputeRequirement();
      auto const workspaceColumns = std::max(0, state.terminalColumns - entryPointsPtr->requirement().min_x - 4);
      auto activityPtr = ftxui::Element{};
      std::int32_t activityColumns = 0;

      if (hasVisibleActivity(state.activityStatus))
      {
        auto const& compact = state.activityStatus->compact;
        auto const urgent = compact.kind == uimodel::ActivityStatusKind::Warning ||
                            compact.kind == uimodel::ActivityStatusKind::Error ||
                            compact.kind == uimodel::ActivityStatusKind::Processing;
        auto const narrowFilter = !state.filterDraft.empty() && state.terminalColumns < kExpandedWorkspaceHintColumns;

        if (!narrowFilter)
        {
          activityColumns = std::min(kMaximumActivityColumns, workspaceColumns / 3);
          activityPtr = activityStatusSlot(state, 0) | size(WIDTH, EQUAL, activityColumns);
        }
        else if (urgent && workspaceColumns >= 16)
        {
          activityColumns = 3;
          activityPtr = statusAction(text(compact.kind == uimodel::ActivityStatusKind::Processing ? " … " : " ! ") |
                                       activityKindColor(compact.kind) | bold,
                                     KeyAction::ToggleNotifications,
                                     state);

          if (state.activityStatusHovered)
          {
            activityPtr = std::move(activityPtr) | style::buttonHover();
          }

          if (state.activityStatusBox != nullptr)
          {
            activityPtr = std::move(activityPtr) | reflect(*state.activityStatusBox);
          }
        }

        if (activityPtr == nullptr && state.activityStatusBox != nullptr)
        {
          *state.activityStatusBox = kEmptyMouseBox;
        }
      }

      auto parts = Elements{};

      if (activityPtr != nullptr)
      {
        parts.push_back(std::move(activityPtr));
      }

      parts.push_back(text(" "));
      parts.push_back(workspaceShortcuts(textCatalog, state, keymapPlan, workspaceColumns - activityColumns));
      parts.push_back(filler());
      parts.push_back(style::mutedSeparator());
      parts.push_back(std::move(entryPointsPtr));
      return hbox(std::move(parts));
    }
  } // namespace

  std::string_view activityKindLabel(uimodel::ActivityStatusKind const kind)
  {
    switch (kind)
    {
      case uimodel::ActivityStatusKind::Processing: return "work";
      case uimodel::ActivityStatusKind::Info: return "info";
      case uimodel::ActivityStatusKind::Warning: return "warn";
      case uimodel::ActivityStatusKind::Error: return "error";
      case uimodel::ActivityStatusKind::Idle: return "idle";
    }

    return "info";
  }

  ftxui::Decorator activityKindColor(uimodel::ActivityStatusKind const kind)
  {
    switch (kind)
    {
      case uimodel::ActivityStatusKind::Processing:
      case uimodel::ActivityStatusKind::Info: return style::accent();
      case uimodel::ActivityStatusKind::Warning: return style::warning();
      case uimodel::ActivityStatusKind::Error: return style::danger();
      case uimodel::ActivityStatusKind::Idle: return ftxui::nothing;
    }

    return style::accent();
  }

  uimodel::ActivityStatusKind activityKindForSeverity(rt::NotificationSeverity const severity)
  {
    switch (severity)
    {
      case rt::NotificationSeverity::Info: return uimodel::ActivityStatusKind::Info;
      case rt::NotificationSeverity::Warning: return uimodel::ActivityStatusKind::Warning;
      case rt::NotificationSeverity::Error: return uimodel::ActivityStatusKind::Error;
    }

    return uimodel::ActivityStatusKind::Info;
  }

  std::string activityProgressRail(double const fraction, std::int32_t const columns)
  {
    if (columns <= 0)
    {
      return {};
    }

    auto const clamped = std::clamp(fraction, 0.0, 1.0);
    auto const filled = static_cast<std::int32_t>(std::lround(clamped * static_cast<double>(columns)));
    auto rail = std::string{};
    rail.reserve(static_cast<std::size_t>(columns) + 2U);
    rail.push_back('[');
    rail.append(static_cast<std::size_t>(std::clamp(filled, 0, columns)), '=');
    rail.append(static_cast<std::size_t>(std::max(0, columns - filled)), '-');
    rail.push_back(']');
    return rail;
  }

  ftxui::Element activityCompactLine(uimodel::ActivityCompactState const& compact, bool const plain)
  {
    auto parts = ftxui::Elements{};
    auto color = plain ? ftxui::nothing : activityKindColor(compact.kind);
    parts.push_back(ftxui::text(std::string{activityKindLabel(compact.kind)}) | color | ftxui::bold);
    parts.push_back(ftxui::text(" "));
    parts.push_back(ftxui::text(compact.text) | color);

    if (compact.optProgressFraction)
    {
      parts.push_back(ftxui::text(" "));
      parts.push_back(ftxui::text(activityProgressRail(*compact.optProgressFraction, 8)) | color);
    }

    if (compact.hasDetails)
    {
      parts.push_back(ftxui::text(" …") | ftxui::dim);
    }
    else if (compact.dismissible)
    {
      parts.push_back(ftxui::text(" ×") | ftxui::dim);
    }

    return ftxui::hbox(std::move(parts));
  }

  bool hasVisibleActivity(uimodel::ActivityStatusViewState const* const state) noexcept
  {
    return state != nullptr && state->compact.kind != uimodel::ActivityStatusKind::Idle && !state->compact.text.empty();
  }

  ftxui::Element statusBar(i18n::MessageCatalog const& textCatalog,
                           StatusBarViewState const& state,
                           KeymapPlan const& keymapPlan)
  {
    using namespace ftxui;

    if (state.settingsButtonBox != nullptr)
    {
      *state.settingsButtonBox = kEmptyMouseBox;
    }

    if (state.actionHitRegions != nullptr)
    {
      state.actionHitRegions->clear();
    }

    if (state.cancelSelectionBox != nullptr)
    {
      *state.cancelSelectionBox = kEmptyMouseBox;
    }

    auto const hasActivity = hasVisibleActivity(state.activityStatus);
    auto fallbackShell = ShellInteractionModel{};
    auto const& shell = state.shell == nullptr ? fallbackShell : *state.shell;

    if (shell.inputMode() == ShellInputMode::QuickFilter)
    {
      if (state.activityStatusBox != nullptr)
      {
        *state.activityStatusBox = kEmptyMouseBox;
      }

      return hbox({
               text("/ ") | style::accent() | bold,
               textFieldValue(
                 shell.inputField(), state.inputHitRegions == nullptr ? nullptr : &state.inputHitRegions->inputOrigin) |
                 bold | flex | (state.inputHitRegions == nullptr ? nothing : reflect(state.inputHitRegions->inputBox)),
             }) |
             clear_under;
    }

    if (!hasActivity && state.activityStatusBox != nullptr)
    {
      *state.activityStatusBox = kEmptyMouseBox;
    }

    if (shell.isNavigationFocused() && !shell.isInputActive() && !isModalOverlay(shell.overlay()))
    {
      return hbox(
        {hasActivity ? activityStatusSlot(state) | xflex : filler() | xflex,
         text(std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiShellOverlayLists)}) | style::accent() |
           bold,
         text("  "),
         text(std::string{i18n::requiredText(
           textCatalog,
           state.navigationSearching ? i18n::MessageId::TuiListSearchHint : i18n::MessageId::TuiNavigationKeys)}) |
           dim});
    }

    if (state.visualSelectionActive && !isModalOverlay(shell.overlay()))
    {
      return visualSelectionStatus(textCatalog, state, keymapPlan);
    }

    if (auto const overlay = shell.overlay(); overlay != Overlay::None)
    {
      auto const interactionHint = overlayHint(textCatalog, keymapPlan, overlay);
      auto const contextLabel = std::string{overlayLabel(textCatalog, overlay)};

      return hbox({
        hasActivity ? activityStatusSlot(state) | xflex : filler() | xflex,
        text(" "),
        text(contextLabel) | style::accent() | bold,
        text("  "),
        text(interactionHint) | dim,
      });
    }

    return workspaceStatus(textCatalog, state, keymapPlan);
  }
} // namespace ao::tui
