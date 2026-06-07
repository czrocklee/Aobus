// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/NowPlayingStatusLabel.h"
#include <ao/rt/AppRuntime.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    class NowPlayingStatusComponent final : public ILayoutComponent
    {
    public:
      NowPlayingStatusComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      NowPlayingStatusLabel _widget;
    };

    std::unique_ptr<ILayoutComponent> createNowPlayingStatus(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<NowPlayingStatusComponent>(ctx, node);
    }
  } // namespace

  void registerNowPlayingStatusComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.nowPlaying", .displayName = "Now Playing Status", .category = "Status"}, createNowPlayingStatus);
  }
} // namespace ao::gtk::layout
