// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ActionTypes.h>

#include <optional>
#include <string_view>
#include <vector>

namespace ao::uimodel::layout
{
  class ActionCatalog final
  {
  public:
    bool registerActionDescriptor(ActionDescriptor descriptor);

    std::optional<ActionDescriptor> descriptor(std::string_view id) const;
    std::vector<ActionDescriptor> descriptors() const;

    bool canBind(std::string_view id, ActionBindingContext const& ctx) const;
    bool tryBind(std::string_view id, ActionBindingContext const& ctx) const;

  private:
    std::vector<ActionDescriptor> _descriptors;
  };
}
