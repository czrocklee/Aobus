// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentCatalog.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::layout
{
  ComponentDescriptor componentDescriptorWithActionProperties(ComponentDescriptor descriptor)
  {
    auto const& descriptorPolicy = descriptor.actionPolicy;

    auto const inject = [&descriptor, &descriptorPolicy](std::string_view name, std::string_view label, ActionSlot slot)
    {
      if (auto it = std::ranges::find_if(
            descriptor.props, [name](PropertyDescriptor const& prop) { return prop.name == name; });
          it != descriptor.props.end())
      {
        return;
      }

      auto optDefaultId = std::optional<std::string>{};

      if (auto const id = descriptorPolicy.getDefault(slot); !id.empty())
      {
        optDefaultId = std::string{id};
      }

      descriptor.props.push_back({.name = std::string{name},
                                  .kind = PropertyKind::Enum,
                                  .label = std::string{label},
                                  .defaultValue = LayoutValue{""},
                                  .enumValues = {},
                                  .optActionBinding = ActionBindingProperty{.slot = slot},
                                  .optDefaultActionId = std::move(optDefaultId)});
    };

    if (descriptorPolicy.allows(ActionSlot::PrimaryClick))
    {
      inject(kPrimaryActionProp, "Primary Action", ActionSlot::PrimaryClick);
    }

    if (descriptorPolicy.allows(ActionSlot::PrimaryLongPress))
    {
      inject(kPrimaryLongPressActionProp, "Primary Long Press", ActionSlot::PrimaryLongPress);
    }

    if (descriptorPolicy.allows(ActionSlot::SecondaryClick))
    {
      inject(kSecondaryActionProp, "Secondary Action", ActionSlot::SecondaryClick);
    }

    if (descriptorPolicy.allows(ActionSlot::SecondaryLongPress))
    {
      inject(kSecondaryLongPressActionProp, "Secondary Long Press", ActionSlot::SecondaryLongPress);
    }

    return descriptor;
  }

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
