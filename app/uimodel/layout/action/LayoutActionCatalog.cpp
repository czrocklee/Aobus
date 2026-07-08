// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/action/LayoutActionBinding.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  bool LayoutActionCatalog::registerActionDescriptor(LayoutActionDescriptor descriptor)
  {
    auto const it =
      std::ranges::find_if(_descriptors, [&](auto const& existing) { return existing.id == descriptor.id; });

    if (it != _descriptors.end())
    {
      return false;
    }

    _descriptors.push_back(std::move(descriptor));
    return true;
  }

  std::optional<LayoutActionDescriptor> LayoutActionCatalog::descriptor(std::string_view id) const
  {
    auto const it = std::ranges::find_if(_descriptors, [&](auto const& existing) { return existing.id == id; });

    if (it != _descriptors.end())
    {
      return *it;
    }

    return std::nullopt;
  }

  std::vector<LayoutActionDescriptor> LayoutActionCatalog::descriptors() const
  {
    return _descriptors;
  }

  bool LayoutActionCatalog::canBind(std::string_view id, LayoutActionBindingContext const& ctx) const
  {
    auto const optDescriptor = descriptor(id);

    if (!optDescriptor)
    {
      return false;
    }

    auto const& desc = *optDescriptor;

    if (!ctx.hasAnchor && desc.capabilities.has(LayoutActionCapability::RequiresAnchor))
    {
      return false;
    }

    if (!ctx.hasFocusedView && desc.capabilities.has(LayoutActionCapability::RequiresFocusedView))
    {
      return false;
    }

    return true;
  }

  bool LayoutActionCatalog::tryBind(std::string_view id, LayoutActionBindingContext const& ctx) const
  {
    if (id == "none" || id.empty())
    {
      return false;
    }

    return canBind(id, ctx);
  }
} // namespace ao::uimodel
