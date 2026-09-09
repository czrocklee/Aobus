// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Keymap.h"
#include "MouseBindings.h"
#include "ShellInteractionModel.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <list>
#include <string>
#include <string_view>

namespace ao::tui
{
  inline constexpr std::int32_t kDefaultStatusBarColumns = 140;
  /// The rows the bottom status bar claims from the root layout.
  inline constexpr std::int32_t kStatusBarRows = 1;

  struct StatusActionHitRegion final
  {
    KeyAction action = KeyAction::ShowHelp;
    ftxui::Box box = kEmptyMouseBox;
  };

  struct CompletionHitRegions;

  struct StatusBarViewState final
  {
    uimodel::ActivityStatusViewState const* activityStatus = nullptr;
    std::int32_t terminalColumns = kDefaultStatusBarColumns;
    std::string filterDraft{};
    bool filterInvalid = false;
    bool visualSelectionActive = false;
    bool navigationSearching = false;
    ShellInteractionModel const* shell = nullptr;
    ftxui::Box* activityStatusBox = nullptr;
    ftxui::Box* cancelSelectionBox = nullptr;
    bool activityStatusHovered = false;
    ftxui::Box* settingsButtonBox = nullptr;
    bool settingsHovered = false;
    std::list<StatusActionHitRegion>* actionHitRegions = nullptr;
    CompletionHitRegions* inputHitRegions = nullptr;
  };

  std::string_view activityKindLabel(uimodel::ActivityStatusKind kind);
  ftxui::Decorator activityKindColor(uimodel::ActivityStatusKind kind);
  uimodel::ActivityStatusKind activityKindForSeverity(rt::NotificationSeverity severity);
  std::string activityProgressRail(double fraction, std::int32_t columns);
  ftxui::Element activityCompactLine(uimodel::ActivityCompactState const& compact, bool plain = false);
  bool hasVisibleActivity(uimodel::ActivityStatusViewState const* state) noexcept;
  ftxui::Element statusBar(i18n::MessageCatalog const& textCatalog,
                           StatusBarViewState const& state,
                           KeymapPlan const& keymapPlan);
} // namespace ao::tui
