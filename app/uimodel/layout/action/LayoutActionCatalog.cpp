// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

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
} // namespace ao::uimodel
