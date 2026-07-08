// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/PlaybackDetailsWidget.h"
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
    class PlaybackDetailsComponent final : public LayoutComponent
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

    std::unique_ptr<LayoutComponent> createPlaybackDetails(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlaybackDetailsComponent>(ctx, node);
    }
  } // namespace

  void registerPlaybackDetailsComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "status.playbackDetails",
                                .displayName = "Playback Details",
                                .category = LayoutComponentCategory::Status},
                               createPlaybackDetails);
  }
} // namespace ao::gtk::layout
