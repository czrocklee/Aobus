// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPageHost.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief tracks.table
     */
    class TracksTableComponent final : public LayoutComponent
    {
    public:
      TracksTableComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.dependencies.trackPageHost == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageHost missing"));
          return;
        }

        Gtk::Stack& stack = ctx.dependencies.trackPageHost->stack();
        _container.append(stack);
        _container.set_hexpand(true);
        _container.set_vexpand(true);
      }

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container{Gtk::Orientation::VERTICAL};
    };

    std::unique_ptr<LayoutComponent> createTracksTable(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TracksTableComponent>(ctx, node);
    }
  } // namespace

  void registerTracksTableComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "tracks.table",
                                .displayName = "Tracks Table",
                                .category = LayoutComponentCategory::Track,
                                .props = {{.name = "view",
                                           .kind = LayoutPropertyKind::String,
                                           .label = "View Source",
                                           .defaultValue = LayoutValue{"workspace.focused"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTracksTable);
  }
} // namespace ao::gtk::layout
