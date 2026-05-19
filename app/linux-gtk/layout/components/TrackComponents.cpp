// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/components/TrackComponents.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "list/ListSidebarController.h"
#include "track/TrackPageHost.h"
#include "track/TrackPresentationButton.h"
#include "track/TrackQuickFilter.h"
#include <ao/Type.h>

#include <gtkmm/widget.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    ListId allTracksListId()
    {
      return ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    ListId rootParentId()
    {
      return ListId{0};
    }

    /**
     * @brief track.quickFilter component wrapper
     */
    class TrackQuickFilterComponent final : public ILayoutComponent
    {
    public:
      TrackQuickFilterComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime}
      {
        _widget.signalCreateSmartListRequested().connect(
          [&ctx](std::string const& expression)
          {
            if (ctx.track.pageHost != nullptr && ctx.list.sidebarController != nullptr)
            {
              auto parentId = ctx.track.pageHost->activeListId();

              if (parentId == allTracksListId())
              {
                parentId = rootParentId();
              }

              ctx.list.sidebarController->createSmartListFromExpression(parentId, expression);
            }
          });
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      TrackQuickFilter _widget;
    };

    /**
     * @brief track.presentationButton component wrapper
     */
    class TrackPresentationButtonComponent final : public ILayoutComponent
    {
    public:
      TrackPresentationButtonComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime}
      {
        if (ctx.track.pageHost != nullptr)
        {
          _widget.setPresentationStore(&ctx.track.pageHost->presentationStore());
        }
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      TrackPresentationButton _widget;
    };
  } // namespace

  void registerTrackComponents(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.quickFilter",
                                .displayName = "Quick Filter",
                                .category = "Tracks",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<TrackQuickFilterComponent>(ctx, node); });

    registry.registerComponent({.type = "track.presentationButton",
                                .displayName = "Presentation",
                                .category = "Tracks",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<TrackPresentationButtonComponent>(ctx, node); });
  }
} // namespace ao::gtk::layout
