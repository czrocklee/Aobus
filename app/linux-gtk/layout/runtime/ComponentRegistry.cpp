// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"

#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    class ErrorComponent final : public ILayoutComponent
    {
    public:
      explicit ErrorComponent(std::string const& message)
      {
        _label.set_markup("<span foreground='red'><b>[Layout Error]</b></span> " + message);
        _label.add_css_class("ao-layout-error");
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };
  } // namespace

  ComponentRegistry::ComponentRegistry() = default;

  void ComponentRegistry::registerComponent(uimodel::layout::ComponentDescriptor descriptor, ComponentFactory factory)
  {
    auto const type = std::string{descriptor.type};
    _factories[type] = factory;
    _catalog.registerComponentDescriptor(std::move(descriptor));
  }

  std::unique_ptr<ILayoutComponent> ComponentRegistry::create(LayoutContext& ctx,
                                                              uimodel::layout::LayoutNode const& node) const
  {
    if (auto const it = _factories.find(node.type); it != _factories.end())
    {
      return it->second(ctx, node);
    }

    return std::make_unique<ErrorComponent>("Unknown component type: " + node.type);
  }

  std::vector<uimodel::layout::ComponentDescriptor> const& ComponentRegistry::descriptors() const
  {
    return _catalog.descriptors();
  }

  std::optional<uimodel::layout::ComponentDescriptor> ComponentRegistry::descriptor(std::string_view type) const
  {
    return _catalog.descriptor(type);
  }

  uimodel::layout::ComponentCatalog const& ComponentRegistry::catalog() const noexcept
  {
    return _catalog;
  }
} // namespace ao::gtk::layout
