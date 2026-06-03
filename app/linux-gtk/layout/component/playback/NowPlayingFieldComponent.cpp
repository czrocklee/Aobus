// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/NowPlayingFieldLabel.h"
#include <ao/rt/TrackField.h>

#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief Generic now-playing field label component.
     */
    class NowPlayingFieldComponent final : public ILayoutComponent
    {
    public:
      NowPlayingFieldComponent(LayoutContext& ctx, LayoutNode const& node, rt::TrackField field)
        : _label{ctx.runtime,
                 field,
                 [action = node.getProp<std::string>("action", "none")]
                 {
                   if (action == "reveal")
                   {
                     return NowPlayingFieldLabel::Action::Reveal;
                   }

                   if (action == "playPause")
                   {
                     return NowPlayingFieldLabel::Action::PlayPause;
                   }

                   if (action == "filterByField")
                   {
                     return NowPlayingFieldLabel::Action::FilterByField;
                   }

                   return NowPlayingFieldLabel::Action::None;
                 }()}
      {
      }

      Gtk::Widget& widget() override { return _label.widget(); }

    private:
      NowPlayingFieldLabel _label;
    };

    std::unique_ptr<ILayoutComponent> createCurrentTitleLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<NowPlayingFieldComponent>(ctx, node, rt::TrackField::Title);
    }

    std::unique_ptr<ILayoutComponent> createCurrentArtistLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<NowPlayingFieldComponent>(ctx, node, rt::TrackField::Artist);
    }
  } // namespace

  void registerNowPlayingFieldComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.currentTitleLabel",
                                .displayName = "Current Title Label",
                                .category = "Playback",
                                .container = false,
                                .props = {{.name = "action",
                                           .kind = PropertyKind::Enum,
                                           .label = "Action",
                                           .defaultValue = LayoutValue{"none"},
                                           .enumValues = {"none", "reveal", "playPause", "filterByField"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createCurrentTitleLabel);

    registry.registerComponent({.type = "playback.currentArtistLabel",
                                .displayName = "Current Artist Label",
                                .category = "Playback",
                                .container = false,
                                .props = {{.name = "action",
                                           .kind = PropertyKind::Enum,
                                           .label = "Action",
                                           .defaultValue = LayoutValue{"none"},
                                           .enumValues = {"none", "reveal", "playPause", "filterByField"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createCurrentArtistLabel);
  }
} // namespace ao::gtk::layout
