// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>

#include <ao/uimodel/layout/action/LayoutActionSlot.h>

#include <algorithm>
#include <string_view>

namespace ao::uimodel
{
  std::string_view LayoutComponentActionPolicy::defaultAction(LayoutActionSlot const slot) const
  {
    if (auto const it =
          std::ranges::find_if(defaultActionIds, [slot](auto const& entry) { return entry.first == slot; });
        it != defaultActionIds.end())
    {
      return it->second;
    }

    return {};
  }
} // namespace ao::uimodel
