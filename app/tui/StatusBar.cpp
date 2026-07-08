// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusBar.h"

#include "ShellInteractionModel.h"
#include "Style.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
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
      case uimodel::ActivityStatusKind::Success: return "done";
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
      case uimodel::ActivityStatusKind::Success: return style::success();
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

  ftxui::Element statusBar(StatusBarViewState const& state)
  {
    using namespace ftxui;

    constexpr std::int32_t kSingleLineStatusColumns = 120;

    auto workspaceHintPtr = [&]
    {
      auto parts = Elements{};

      if (!state.filterDraft.empty())
      {
        parts.push_back(text("Filter: ") | style::accent() | bold);
        parts.push_back(text(state.filterDraft) | dim);
        parts.push_back(text("  "));
      }

      auto appendSeparator = [&]
      {
        if (!parts.empty())
        {
          parts.push_back(style::mutedSeparator());
        }
      };

      appendSeparator();
      parts.push_back(style::shortcutChip("/", "command"));
      appendSeparator();
      parts.push_back(style::shortcutChip("l", "lists"));
      appendSeparator();
      parts.push_back(style::shortcutChip("v", "view"));
      appendSeparator();
      parts.push_back(style::shortcutChip("n", "notif"));
      appendSeparator();
      parts.push_back(style::shortcutChip("d", "detail"));
      appendSeparator();
      parts.push_back(style::shortcutChip("a", "pipeline"));
      appendSeparator();
      parts.push_back(style::shortcutChip("o", "output"));
      appendSeparator();
      parts.push_back(style::shortcutChip("{ }", "groups"));
      appendSeparator();
      parts.push_back(style::shortcutChip("Ctrl-L", "current"));
      appendSeparator();
      parts.push_back(style::shortcutChip("q", "quit"));
      return hbox(std::move(parts));
    };

    auto const hasActivity = hasVisibleActivity(state.activityStatus);
    auto fallbackShell = ShellInteractionModel{};
    auto const& shell = state.shell == nullptr ? fallbackShell : *state.shell;

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

    auto const overlay = shell.overlay();
    auto const interactionHint = std::string{overlayHint(overlay)};
    auto const contextLabel = overlay == Overlay::None ? std::string{} : overlayLabel(overlay);

    auto hint = std::string{};

    if (!state.filterDraft.empty())
    {
      hint = std::format("Filter: {}  ", state.filterDraft);
    }

    hint += interactionHint;

    if (overlay != Overlay::None)
    {
      return hbox({
        leftStatusAreaPtr(),
        text(" "),
        text(contextLabel) | style::accent() | bold,
        text("  "),
        text(hint) | dim,
      });
    }

    if (state.terminalColumns >= kSingleLineStatusColumns)
    {
      return hbox({
        leftStatusAreaPtr(),
        text(" "),
        workspaceHintPtr(),
      });
    }

    return vbox({
      hbox({
        leftStatusAreaPtr(),
      }),
      hbox({
        workspaceHintPtr() | flex,
      }),
    });
  }
} // namespace ao::tui
