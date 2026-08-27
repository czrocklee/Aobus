// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusBar.h"

#include "CommandCompletion.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui
{
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

  ftxui::Element statusBar(i18n::MessageCatalog const& textCatalog, StatusBarViewState const& state)
  {
    using namespace ftxui;

    constexpr std::int32_t kExpandedWorkspaceHintColumns = 100;

    auto workspaceHintPtr = [&]
    {
      auto parts = Elements{};

      auto appendSeparator = [&]
      {
        if (!parts.empty())
        {
          parts.push_back(style::mutedSeparator());
        }
      };

      auto appendChip = [&](std::string_view const key, std::string_view const label)
      {
        appendSeparator();
        parts.push_back(style::shortcutChip(key, label));
      };

      // The key comes from the one binding declaration, so a rebound action
      // cannot leave the bar advertising the key it used to answer to. The
      // words are the bar's own: they name the destination, not the command.
      auto appendCommandChip = [&](CommandAction const action, std::string_view const label)
      { appendChip(shortcutFor(action), label); };

      appendChip("/",
                 state.filterDraft.empty() ? i18n::requiredText(textCatalog, i18n::MessageId::TuiShellFilterLabel)
                                           : std::string_view{state.filterDraft});

      if (!state.filterDraft.empty())
      {
        appendCommandChip(
          CommandAction::ClearFilter, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusClearFilter));
      }

      // Opening the text inputs is not a command, so it is named here rather
      // than added to the key-binding declaration.
      appendChip(":", i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusCommand));

      if (state.terminalColumns >= kExpandedWorkspaceHintColumns)
      {
        appendCommandChip(
          CommandAction::OpenLists, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusLists));
        appendCommandChip(
          CommandAction::OpenPresentationPanel, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusView));
        appendCommandChip(
          CommandAction::OpenDetail, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusDetail));
      }

      appendCommandChip(CommandAction::ShowHelp, i18n::requiredText(textCatalog, i18n::MessageId::TuiShellStatusHelp));
      return hbox(std::move(parts));
    };

    auto const hasActivity = hasVisibleActivity(state.activityStatus);
    auto fallbackShell = ShellInteractionModel{};
    auto const& shell = state.shell == nullptr ? fallbackShell : *state.shell;

    if (shell.inputMode() == ShellInputMode::QuickFilter)
    {
      if (state.activityStatusBox != nullptr)
      {
        *state.activityStatusBox = {};
      }

      return hbox({
               text("/ ") | style::accent() | bold,
               text(shell.inputDraft()) | bold,
               text(commandCompletionSuffix(shell)) | dim,
               text("_") | style::accent() | bold,
               filler(),
             }) |
             clear_under;
    }

    if (!hasActivity && state.activityStatusBox != nullptr)
    {
      *state.activityStatusBox = {};
    }

    auto statusSlotPtr = [&]
    {
      auto bodyPtr = activityCompactLine(state.activityStatus->compact, state.activityStatusHovered);

      if (state.activityStatusHovered)
      {
        bodyPtr = std::move(bodyPtr) | style::buttonHover();
      }

      auto slotPtr = style::statusSlot(std::move(bodyPtr));

      if (state.activityStatusBox != nullptr)
      {
        slotPtr = std::move(slotPtr) | reflect(*state.activityStatusBox);
      }

      return slotPtr;
    };

    auto leftStatusAreaPtr = [&] { return hasActivity ? statusSlotPtr() | xflex : filler() | xflex; };

    if (auto const overlay = shell.overlay(); overlay != Overlay::None)
    {
      auto const interactionHint = std::string{overlayHint(textCatalog, overlay)};
      auto const contextLabel = std::string{overlayLabel(textCatalog, overlay)};

      return hbox({
        leftStatusAreaPtr(),
        text(" "),
        text(contextLabel) | style::accent() | bold,
        text("  "),
        text(interactionHint) | dim,
      });
    }

    return hbox({
      leftStatusAreaPtr(),
      text(" "),
      workspaceHintPtr(),
    });
  }
} // namespace ao::tui
