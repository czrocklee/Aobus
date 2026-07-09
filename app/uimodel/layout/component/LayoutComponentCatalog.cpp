// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/action/LayoutActionBinding.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  LayoutComponentDescriptor componentDescriptorWithActionProperties(LayoutComponentDescriptor descriptor)
  {
    auto const& descriptorPolicy = descriptor.actionPolicy;

    auto const inject = [&descriptor, &descriptorPolicy](
                          std::string_view name, std::string_view label, LayoutActionSlot slot)
    {
      if (auto it = std::ranges::find_if(
            descriptor.props, [name](LayoutPropertyDescriptor const& prop) { return prop.name == name; });
          it != descriptor.props.end())
      {
        return;
      }

      auto optDefaultId = std::optional<std::string>{};

      if (auto const id = descriptorPolicy.defaultAction(slot); !id.empty())
      {
        optDefaultId = std::string{id};
      }

      descriptor.props.push_back({.name = std::string{name},
                                  .kind = LayoutPropertyKind::Enum,
                                  .label = std::string{label},
                                  .defaultValue = LayoutValue{""},
                                  .enumValues = {},
                                  .optActionBinding = LayoutActionBindingProperty{.slot = slot},
                                  .optDefaultActionId = std::move(optDefaultId)});
    };

    if (descriptorPolicy.isSlotAllowed(LayoutActionSlot::PrimaryClick))
    {
      inject(kPrimaryActionProp, "Primary Action", LayoutActionSlot::PrimaryClick);
    }

    if (descriptorPolicy.isSlotAllowed(LayoutActionSlot::PrimaryLongPress))
    {
      inject(kPrimaryLongPressActionProp, "Primary Long Press", LayoutActionSlot::PrimaryLongPress);
    }

    if (descriptorPolicy.isSlotAllowed(LayoutActionSlot::SecondaryClick))
    {
      inject(kSecondaryActionProp, "Secondary Action", LayoutActionSlot::SecondaryClick);
    }

    if (descriptorPolicy.isSlotAllowed(LayoutActionSlot::SecondaryLongPress))
    {
      inject(kSecondaryLongPressActionProp, "Secondary Long Press", LayoutActionSlot::SecondaryLongPress);
    }

    return descriptor;
  }

  bool LayoutComponentCatalog::registerComponentDescriptor(LayoutComponentDescriptor descriptor)
  {
    if (_descriptorIndexMap.contains(descriptor.type))
    {
      return false;
    }

    _descriptorIndexMap[descriptor.type] = _descriptors.size();
    _descriptors.push_back(std::move(descriptor));
    return true;
  }

  std::vector<LayoutComponentDescriptor> const& LayoutComponentCatalog::descriptors() const noexcept
  {
    return _descriptors;
  }

  std::optional<LayoutComponentDescriptor> LayoutComponentCatalog::descriptor(std::string_view type) const
  {
    if (auto const it = _descriptorIndexMap.find(type); it != _descriptorIndexMap.end())
    {
      return _descriptors[it->second];
    }

    return std::nullopt;
  }
} // namespace ao::uimodel
