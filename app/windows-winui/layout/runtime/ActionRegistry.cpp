// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include "pch.h"

#include <string>
#include <string_view>
#include <utility>

namespace ao::winui::layout
{
  void ActionRegistry::registerAction(std::string_view const id, ActionHandler handler)
  {
    _handlers.insert_or_assign(std::string{id}, std::move(handler));
  }

  bool ActionRegistry::contains(std::string_view const id) const
  {
    return _handlers.contains(id);
  }

  bool ActionRegistry::invoke(std::string_view const id, ActionContext const& context) const
  {
    auto const it = _handlers.find(id);

    if (it == _handlers.end() || !it->second)
    {
      return false;
    }

    it->second(context);
    return true;
  }
} // namespace ao::winui::layout
