// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/PlaybackDetailsWidget.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    class PlaybackDetailsComponent final : public ILayoutComponent
    {
    public:
      PlaybackDetailsComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      PlaybackDetailsWidget _widget;
    };

    std::unique_ptr<ILayoutComponent> createPlaybackDetails(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlaybackDetailsComponent>(ctx, node);
    }
  } // namespace

  void registerPlaybackDetailsComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.playbackDetails", .displayName = "Playback Details", .category = ComponentCategory::Status},
      createPlaybackDetails);
  }
} // namespace ao::gtk::layout
