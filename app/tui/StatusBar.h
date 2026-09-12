// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "GoToMenu.h"
#include "Keymap.h"
#include "MouseBindings.h"
#include "PanelResize.h"
#include "ShellInteractionModel.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  struct PlaybackSuccessionSnapshot;
}

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
    bool hasTrackSelection = false;
    bool visualSelectionActive = false;
    bool navigationSearching = false;
    bool navigationDocked = false;
    std::optional<PanelDivider> optResizingDivider{};
    ShellInteractionModel const* shell = nullptr;
    ftxui::Box* activityStatusBox = nullptr;
    ftxui::Box* cancelSelectionBox = nullptr;
    ftxui::Box* navigationSearchBox = nullptr;
    bool activityStatusHovered = false;
    ftxui::Box* settingsButtonBox = nullptr;
    bool settingsHovered = false;
    rt::PlaybackSuccessionSnapshot const* hoveredPlaybackMode = nullptr;
    std::list<StatusActionHitRegion>* actionHitRegions = nullptr;
    CompletionHitRegions* inputHitRegions = nullptr;
    GoToMenuState goToState{};
    GoToMenuHitRegions* goToHitRegions = nullptr;
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
