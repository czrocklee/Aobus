// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/rt/ProjectionTypes.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    class TrackSelectionRegionComponent final : public ILayoutComponent
    {
    public:
      TrackSelectionRegionComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 0}, _showWhen{node.getProp<std::string>("showWhen", "any")}
      {
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);
          _box.append(childPtr->widget());
          _children.push_back(std::move(childPtr));
        }

        if (ctx.track.detailScope != nullptr)
        {
          _scopeConn = ctx.track.detailScope->signalSnapshotChanged().connect([this](auto const& snap)
                                                                              { updateVisibility(snap); });
          updateVisibility(ctx.track.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      void updateVisibility(rt::TrackDetailSnapshot const& snap)
      {
        bool visible = false;

        if (_showWhen == "none")
        {
          visible = (snap.selectionKind == rt::SelectionKind::None);
        }
        else if (_showWhen == "single")
        {
          visible = (snap.selectionKind == rt::SelectionKind::Single);
        }
        else if (_showWhen == "multiple")
        {
          visible = (snap.selectionKind == rt::SelectionKind::Multiple);
        }
        else if (_showWhen == "any")
        {
          visible = (snap.selectionKind != rt::SelectionKind::None);
        }

        _box.set_visible(visible);
      }

      Gtk::Box _box;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
      std::string _showWhen;
      sigc::connection _scopeConn;
    };

    std::unique_ptr<ILayoutComponent> createTrackSelectionRegion(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackSelectionRegionComponent>(ctx, node);
    }
  } // namespace

  void registerTrackSelectionRegionComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.selectionRegion",
       .displayName = "Selection Region",
       .category = "Tracks",
       .container = true,
       .props =
         {{.name = "showWhen", .kind = PropertyKind::String, .label = "Show When", .defaultValue = LayoutValue{"any"}}},
       .layoutProps = {},
       .minChildren = 1,
       .optMaxChildren = 0},
      createTrackSelectionRegion);
  }
} // namespace ao::gtk::layout
