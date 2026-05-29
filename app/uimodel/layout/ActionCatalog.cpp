// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ActionCatalog.h>
#include <ao/uimodel/layout/ActionTypes.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::layout
{
  bool ActionCatalog::registerActionDescriptor(ActionDescriptor descriptor)
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

  std::optional<ActionDescriptor> ActionCatalog::descriptor(std::string_view id) const
  {
    auto const it = std::ranges::find_if(_descriptors, [&](auto const& existing) { return existing.id == id; });

    if (it != _descriptors.end())
    {
      return *it;
    }

    return std::nullopt;
  }

  std::vector<ActionDescriptor> ActionCatalog::descriptors() const
  {
    return _descriptors;
  }

  bool ActionCatalog::canBind(std::string_view id, ActionBindingContext const& ctx) const
  {
    auto const optDescriptor = descriptor(id);

    if (!optDescriptor)
    {
      return false;
    }

    auto const& desc = *optDescriptor;

    if (!ctx.hasAnchor && desc.capabilities.has(ActionCapability::RequiresAnchor))
    {
      return false;
    }

    if (!ctx.hasFocusedView && desc.capabilities.has(ActionCapability::RequiresFocusedView))
    {
      return false;
    }

    return true;
  }

  bool ActionCatalog::tryBind(std::string_view id, ActionBindingContext const& ctx) const
  {
    if (id == "none" || id.empty())
    {
      return false;
    }

    return canBind(id, ctx);
  }
} // namespace ao::uimodel::layout
