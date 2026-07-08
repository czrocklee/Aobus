// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/stack.h>
#include <gtkmm/stackswitcher.h>
#include <gtkmm/widget.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief A stack of tabs (Gtk::Stack).
     */
    class TabsComponent final : public LayoutComponent
    {
    public:
      TabsComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL}
      {
        if (node.children.empty())
        {
          _errorPtr = std::make_unique<Gtk::Label>();
          _errorPtr->set_markup("<span foreground='red'><b>[Layout Error]</b> tabs require at least 1 child</span>");
          _errorPtr->add_css_class("ao-layout-error");
          return;
        }

        _switcher.set_stack(_stack);
        _box.append(_switcher);
        _box.append(_stack);
        _stack.set_vexpand(true);

        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);

          auto const title =
            childNode.layoutOr<std::string>("title", !childNode.id.empty() ? childNode.id : "[Untitled]");
          auto const name = !childNode.id.empty() ? childNode.id : childNode.type;

          auto const pagePtr = _stack.add(childPtr->widget(), name, title);

          if (auto const it = childNode.layout.find("icon"); it != childNode.layout.end())
          {
            pagePtr->set_icon_name(it->second.asString());
          }

          _children.push_back(std::move(childPtr));
        }
      }

      Gtk::Widget& widget() override
      {
        return (_errorPtr != nullptr) ? static_cast<Gtk::Widget&>(*_errorPtr) : static_cast<Gtk::Widget&>(_box);
      }

    private:
      Gtk::Box _box;
      Gtk::StackSwitcher _switcher;
      Gtk::Stack _stack;
      std::unique_ptr<Gtk::Label> _errorPtr;
      std::vector<std::unique_ptr<LayoutComponent>> _children;
    };

    std::unique_ptr<LayoutComponent> createTabs(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TabsComponent>(ctx, node);
    }
  } // namespace

  void registerTabsComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "tabs",
       .displayName = "Tabs",
       .category = LayoutComponentCategory::Container,
       .props = {},
       .layoutProps =
         {{.name = "title", .kind = LayoutPropertyKind::String, .label = "Tab Title", .defaultValue = LayoutValue{""}},
          {.name = "icon", .kind = LayoutPropertyKind::String, .label = "Tab Icon", .defaultValue = LayoutValue{""}}},
       .minChildren = 1,
       .optMaxChildren = std::nullopt},
      createTabs);
  }
} // namespace ao::gtk::layout
