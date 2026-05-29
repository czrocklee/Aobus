// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ComponentCatalog.h>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::layout
{
  bool ComponentCatalog::registerComponentDescriptor(ComponentDescriptor descriptor)
  {
    if (_descriptorIndexMap.contains(descriptor.type))
    {
      return false;
    }

    _descriptorIndexMap[descriptor.type] = _descriptors.size();
    _descriptors.push_back(std::move(descriptor));
    return true;
  }

  std::vector<ComponentDescriptor> const& ComponentCatalog::descriptors() const noexcept
  {
    return _descriptors;
  }

  std::optional<ComponentDescriptor> ComponentCatalog::descriptor(std::string_view type) const
  {
    if (auto const it = _descriptorIndexMap.find(type); it != _descriptorIndexMap.end())
    {
      return _descriptors[it->second];
    }

    return std::nullopt;
  }
} // namespace ao::uimodel::layout
