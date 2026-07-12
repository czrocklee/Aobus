// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPageHost.h"
#include "track/TrackQuickFilter.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/track/TrackPageRoute.h>

#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <memory>
#include <string>
#include <utility>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief track.quickFilter component wrapper
     */
    class TrackQuickFilterComponent final : public LayoutComponent
    {
    public:
      TrackQuickFilterComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime, ctx.timeoutScheduler}
      {
        auto* const pageHost = ctx.dependencies.trackPageHost;
        auto createSmartListFromExpression = ctx.dependencies.createSmartListFromExpression;

        if (pageHost == nullptr || !createSmartListFromExpression)
        {
          return;
        }

        _createSmartListConn = _widget.signalCreateSmartListRequested().connect(
          [createSmartListFromExpression = std::move(createSmartListFromExpression),
           pageHost](std::string const& expression) mutable
          {
            auto const parentListId = uimodel::smartListParentIdFromPage(pageHost->activeListId());
            createSmartListFromExpression(parentListId, expression);
          });
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      TrackQuickFilter _widget;
      sigc::scoped_connection _createSmartListConn;
    };

    std::unique_ptr<LayoutComponent> createTrackQuickFilter(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackQuickFilterComponent>(ctx, node);
    }
  } // namespace

  void registerTrackQuickFilterComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.quickFilter",
                                .displayName = "Quick Filter",
                                .category = LayoutComponentCategory::Track,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTrackQuickFilter);
  }
} // namespace ao::gtk::layout
