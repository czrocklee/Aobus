// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/NowPlayingStatusLabel.h"
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
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
      NowPlayingStatusComponent(rt::PlaybackService& playback, i18n::MessageCatalog const& textCatalog)
        : _widget{playback, textCatalog}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      NowPlayingStatusLabel _widget;
    };
  } // namespace

  void registerNowPlayingStatusComponent(ComponentRegistry& registry,
                                         rt::PlaybackService& playback,
                                         i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent({.id = "status.nowPlaying",
                                .displayName = "Now Playing Status",
                                .category = ComponentCategory::Status,
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [&playback, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
                               { return std::make_unique<NowPlayingStatusComponent>(playback, textCatalog); });
  }
} // namespace ao::gtk::layout
