// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/TrackQuickFilter.h"

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief track.quickFilter component wrapper
     */
    class TrackQuickFilterComponent final : public ILayoutComponent
    {
    public:
      TrackQuickFilterComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime}
      {
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      TrackQuickFilter _widget;
    };

    std::unique_ptr<ILayoutComponent> createTrackQuickFilter(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackQuickFilterComponent>(ctx, node);
    }
  } // namespace

  void registerTrackQuickFilterComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.quickFilter",
                                .displayName = "Quick Filter",
                                .category = "Tracks",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTrackQuickFilter);
  }
} // namespace ao::gtk::layout
