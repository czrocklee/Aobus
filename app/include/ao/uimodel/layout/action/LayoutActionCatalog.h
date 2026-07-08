// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

#include <optional>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  struct LayoutActionBindingContext;

  class LayoutActionCatalog final
  {
  public:
    bool registerActionDescriptor(LayoutActionDescriptor descriptor);

    std::optional<LayoutActionDescriptor> descriptor(std::string_view id) const;
    std::vector<LayoutActionDescriptor> descriptors() const;

    bool canBind(std::string_view id, LayoutActionBindingContext const& ctx) const;
    bool tryBind(std::string_view id, LayoutActionBindingContext const& ctx) const;

  private:
    std::vector<LayoutActionDescriptor> _descriptors = {};
  };
} // namespace ao::uimodel
