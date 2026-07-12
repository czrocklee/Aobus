// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/NowPlayingStatusLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class NowPlayingStatusComponent final : public LayoutComponent
    {
    public:
      NowPlayingStatusComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      NowPlayingStatusLabel _widget;
    };

    std::unique_ptr<LayoutComponent> createNowPlayingStatus(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<NowPlayingStatusComponent>(ctx, node);
    }
  } // namespace

  void registerNowPlayingStatusComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.nowPlaying", .displayName = "Now Playing Status", .category = LayoutComponentCategory::Status},
      createNowPlayingStatus);
  }
} // namespace ao::gtk::layout
