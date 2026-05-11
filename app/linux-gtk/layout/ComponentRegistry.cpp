// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ComponentRegistry.h"

#include <gtkmm/label.h>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief Internal fallback component for unknown types or errors.
     */
    class ErrorComponent final : public ILayoutComponent
    {
    public:
      explicit ErrorComponent(std::string const& message)
      {
        _label.set_markup("<span foreground='red'><b>[Layout Error]</b></span> " + message);

        int const margin = 10;
        _label.set_margin(margin);
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };
  } // namespace

  ComponentRegistry::ComponentRegistry() = default;

  void ComponentRegistry::registerComponent(ComponentDescriptor descriptor, ComponentFactory factory)
  {
    std::string const type = descriptor.type;
    _factories[type] = factory;

    if (auto const it = _descriptorIndexMap.find(type); it != _descriptorIndexMap.end())
    {
      _descriptors[it->second] = std::move(descriptor);
    }
    else
    {
      _descriptorIndexMap[type] = _descriptors.size();
      _descriptors.push_back(std::move(descriptor));
    }
  }

  std::unique_ptr<ILayoutComponent> ComponentRegistry::create(ComponentContext& ctx, LayoutNode const& node) const
  {
    if (auto const it = _factories.find(node.type); it != _factories.end())
    {
      return it->second(ctx, node);
    }

    return std::make_unique<ErrorComponent>("Unknown component type: " + node.type);
  }

  std::vector<ComponentDescriptor> const& ComponentRegistry::getDescriptors() const
  {
    return _descriptors;
  }

  std::optional<ComponentDescriptor> ComponentRegistry::getDescriptor(std::string const& type) const
  {
    if (auto const it = _descriptorIndexMap.find(type); it != _descriptorIndexMap.end())
    {
      return _descriptors[it->second];
    }
    return std::nullopt;
  }
} // namespace ao::gtk::layout
