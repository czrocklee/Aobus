// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPageHost.h"
#include "track/TrackQuickFilter.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/track/TrackPageRoute.h>

#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <functional>
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
      TrackQuickFilterComponent(rt::AppRuntime& runtime,
                                TrackPageHost* pageHost,
                                std::function<void(ao::ListId, std::string)> const& createSmartListFromExpression,
                                i18n::MessageCatalog const& textCatalog,
                                LayoutBuildContext const& ctx)
        : _widget{runtime, textCatalog, ctx.timeoutScheduler}
      {
        if (pageHost == nullptr || !createSmartListFromExpression)
        {
          return;
        }

        _createSmartListConn = _widget.signalCreateSmartListRequested().connect(
          [createSmartList = createSmartListFromExpression, pageHost](std::string const& expression)
          {
            auto const parentListId = uimodel::smartListParentIdFromPage(pageHost->activeListId());
            createSmartList(parentListId, expression);
          });
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      TrackQuickFilter _widget;
      sigc::scoped_connection _createSmartListConn;
    };
  } // namespace

  void registerTrackQuickFilterComponent(ComponentRegistry& registry,
                                         rt::AppRuntime& runtime,
                                         TrackPageHost* trackPageHost,
                                         std::function<void(ao::ListId, std::string)> createSmartListFromExpression,
                                         i18n::MessageCatalog const& textCatalog)
  {
    registry.registerSharedComponent(
      "track.quickFilter",
      [&runtime, trackPageHost, createFn = std::move(createSmartListFromExpression), textCatalog](
        LayoutBuildContext const& ctx, LayoutNode const& /*node*/)
      { return std::make_unique<TrackQuickFilterComponent>(runtime, trackPageHost, createFn, textCatalog, ctx); });
  }
} // namespace ao::gtk::layout
