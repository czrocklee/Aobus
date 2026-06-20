// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailScope.h"

#include "TrackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    // Helper to walk widget tree and reset scrolled windows
    void resetScrollAdjustments(Gtk::Widget* widget)
    {
      if (widget == nullptr)
      {
        return;
      }

      if (auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(widget); sw != nullptr)
      {
        if (Glib::RefPtr<Gtk::Adjustment> const vadjPtr = sw->get_vadjustment(); vadjPtr != nullptr)
        {
          vadjPtr->set_value(vadjPtr->get_lower());
        }
      }

      for (auto* child = widget->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        resetScrollAdjustments(child);
      }
    }

    class TrackDetailScopeComponent final
      : public ILayoutComponent
      , public ITrackDetailScope
    {
    public:
      TrackDetailScopeComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 0}
        , _projectionPtr{ctx.runtime.views().detailProjection(rt::FocusedViewTarget{},
                                                              ctx.runtime.workspace(),
                                                              ctx.runtime.library().changes())}
      {
        _currentSnap = _projectionPtr->snapshot();

        // Intercept context
        auto* previousScope = ctx.track.detailScope;
        ctx.track.detailScope = this;

        // Build children
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);
          _box.append(childPtr->widget());
          _children.push_back(std::move(childPtr));
        }

        // Restore context
        ctx.track.detailScope = previousScope;

        // Apply styles
        if (auto const it = node.layout.find("cssClasses"); it != node.layout.end())
        {
          if (auto const* const classes = it->second.getIf<std::vector<std::string>>(); classes != nullptr)
          {
            for (auto const& className : *classes)
            {
              _box.add_css_class(className);
            }
          }
          else if (auto const className = it->second.asString(); !className.empty())
          {
            _box.add_css_class(className);
          }
        }

        // Subscribe to projection
        _sub = _projectionPtr->subscribe([this](auto const& snap) { onSnapshot(snap); });
      }

      Gtk::Widget& widget() override { return _box; }

      rt::TrackDetailSnapshot const& snapshot() const override { return _currentSnap; }

      sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() override
      {
        return _signalSnapshotChanged;
      }

    private:
      void onSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        bool const selectionChanged = _currentSnap.trackIds != snap.trackIds;
        _currentSnap = snap;
        _signalSnapshotChanged.emit(snap);

        if (selectionChanged)
        {
          resetScrollAdjustments(&_box);
        }
      }

      Gtk::Box _box;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;

      std::unique_ptr<rt::ITrackDetailProjection> _projectionPtr;
      rt::Subscription _sub;
      rt::TrackDetailSnapshot _currentSnap;

      sigc::signal<void(rt::TrackDetailSnapshot const&)> _signalSnapshotChanged;
    };

    std::unique_ptr<ILayoutComponent> createTrackDetailScope(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackDetailScopeComponent>(ctx, node);
    }
  } // namespace

  void registerTrackDetailScopeComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.detailScope",
       .displayName = "Detail Scope",
       .category = ComponentCategory::Track,
       .props = {},
       .layoutProps = {{.name = "cssClasses", .kind = PropertyKind::String, .label = "CSS Classes"}},
       .minChildren = 1,
       .optMaxChildren = 0},
      createTrackDetailScope);
  }
} // namespace ao::gtk::layout
