// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/PlaybackDetailsWidget.h"
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
    class PlaybackDetailsComponent final : public LayoutComponent
    {
    public:
      PlaybackDetailsComponent(rt::PlaybackService& playback, i18n::MessageCatalog const& textCatalog)
        : _widget{playback, textCatalog}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      PlaybackDetailsWidget _widget;
    };
  } // namespace

  void registerPlaybackDetailsComponent(ComponentRegistry& registry,
                                        rt::PlaybackService& playback,
                                        i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent({.id = "status.playbackDetails",
                                .displayName = "Playback Details",
                                .category = ComponentCategory::Status,
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [&playback, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
                               { return std::make_unique<PlaybackDetailsComponent>(playback, textCatalog); });
  }
} // namespace ao::gtk::layout
