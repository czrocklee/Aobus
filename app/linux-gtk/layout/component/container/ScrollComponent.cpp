// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief A scrollable container component (Gtk::ScrolledWindow).
     */
    class ScrollComponent final : public LayoutComponent
    {
    public:
      ScrollComponent(LayoutBuildContext& ctx, LayoutNode const& node)
      {
        if (node.children.size() != 1)
        {
          _errorPtr = std::make_unique<Gtk::Label>();
          _errorPtr->set_markup("<span foreground='red'><b>[Layout Error]</b> scroll requires exactly 1 child</span>");
          _errorPtr->add_css_class("ao-layout-error");
          return;
        }

        _childPtr = ctx.registry.create(ctx, node.children[0]);
        _sw.set_child(_childPtr->widget());

        auto hpolicy = Gtk::PolicyType::AUTOMATIC;
        auto const hscrollPolicy = node.propertyOr<std::string>("hscrollPolicy", "");

        if (hscrollPolicy == "never")
        {
          hpolicy = Gtk::PolicyType::NEVER;
        }
        else if (hscrollPolicy == "always")
        {
          hpolicy = Gtk::PolicyType::ALWAYS;
        }

        auto vpolicy = Gtk::PolicyType::AUTOMATIC;
        auto const vscrollPolicy = node.propertyOr<std::string>("vscrollPolicy", "");

        if (vscrollPolicy == "never")
        {
          vpolicy = Gtk::PolicyType::NEVER;
        }
        else if (vscrollPolicy == "always")
        {
          vpolicy = Gtk::PolicyType::ALWAYS;
        }

        _sw.set_policy(hpolicy, vpolicy);

        _sw.set_min_content_width(static_cast<std::int32_t>(node.propertyOr<std::int64_t>("minContentWidth", -1)));
        _sw.set_min_content_height(static_cast<std::int32_t>(node.propertyOr<std::int64_t>("minContentHeight", -1)));

        _sw.set_propagate_natural_width(node.propertyOr<bool>("propagateNaturalWidth", false));
        _sw.set_propagate_natural_height(node.propertyOr<bool>("propagateNaturalHeight", false));
      }

      Gtk::Widget& widget() override
      {
        return (_errorPtr != nullptr) ? static_cast<Gtk::Widget&>(*_errorPtr) : static_cast<Gtk::Widget&>(_sw);
      }

    private:
      Gtk::ScrolledWindow _sw;
      std::unique_ptr<Gtk::Label> _errorPtr;
      std::unique_ptr<LayoutComponent> _childPtr;
    };

    std::unique_ptr<LayoutComponent> createScroll(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ScrollComponent>(ctx, node);
    }
  } // namespace

  void registerScrollComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "scroll",
                                .displayName = "Scroll Window",
                                .category = LayoutComponentCategory::Container,
                                .props = {{.name = "hscrollPolicy",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "H. Scroll Policy",
                                           .defaultValue = LayoutValue{"automatic"},
                                           .enumValues = {"automatic", "always", "never"}},
                                          {.name = "vscrollPolicy",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "V. Scroll Policy",
                                           .defaultValue = LayoutValue{"automatic"},
                                           .enumValues = {"automatic", "always", "never"}},
                                          {.name = "minContentWidth",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Min Content Width",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}},
                                          {.name = "minContentHeight",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Min Content Height",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}},
                                          {.name = "propagateNaturalWidth",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Propagate Nat. Width",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "propagateNaturalHeight",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Propagate Nat. Height",
                                           .defaultValue = LayoutValue{false}}},
                                .layoutProps = {},
                                .minChildren = 1,
                                .optMaxChildren = 1},
                               createScroll);
  }
} // namespace ao::gtk::layout
