// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationIds.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  class TuiKeymapPlan;

  struct NotificationDetailRowHitRegion final
  {
    rt::NotificationId id{};
    bool dismissible = false;
    ftxui::Box box{};
  };

  std::int32_t notificationCenterPanelColumns(i18n::MessageCatalog const& textCatalog,
                                              uimodel::ActivityStatusViewState const& state,
                                              TuiKeymapPlan const& keymapPlan,
                                              std::int32_t terminalColumns);
  ftxui::Element notificationCenterPanel(i18n::MessageCatalog const& textCatalog,
                                         uimodel::ActivityStatusViewState const& state,
                                         TuiKeymapPlan const& keymapPlan,
                                         std::vector<NotificationDetailRowHitRegion>* rowHitRegions = nullptr,
                                         std::int32_t columns = 0);
} // namespace ao::tui
