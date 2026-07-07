// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ShellModel.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui
{
  inline constexpr std::int32_t kDefaultStatusBarColumns = 140;

  struct StatusBarViewState final
  {
    uimodel::ActivityStatusViewState const* activityStatus = nullptr;
    std::int32_t terminalColumns = kDefaultStatusBarColumns;
    std::string filterDraft{};
    ShellModel const* shell = nullptr;
    ftxui::Box* activityStatusBox = nullptr;
    bool activityStatusHovered = false;
  };

  std::string_view activityKindLabel(uimodel::ActivityStatusKind kind);
  ftxui::Decorator activityKindColor(uimodel::ActivityStatusKind kind);
  uimodel::ActivityStatusKind activityKindForSeverity(rt::NotificationSeverity severity);
  std::string activityProgressRail(double fraction, std::int32_t columns);
  ftxui::Element activityCompactLine(uimodel::ActivityCompactState const& compact, bool plain = false);
  bool hasVisibleActivity(uimodel::ActivityStatusViewState const* state) noexcept;
  ftxui::Element statusBar(StatusBarViewState const& state);
} // namespace ao::tui
