// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/NowPlayingFieldLabel.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief Generic now-playing field label component.
     */
    class NowPlayingFieldComponent final : public LayoutComponent
    {
    public:
      NowPlayingFieldComponent(rt::PlaybackService& playback,
                               rt::WorkspaceService& workspace,
                               i18n::MessageCatalog const& textCatalog,
                               LayoutNode const& node,
                               rt::TrackField const field)
        : _label{playback,
                 workspace,
                 textCatalog,
                 field,
                 [action = node.propertyOr<std::string>("action", "none")]
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
  } // namespace

  void registerNowPlayingFieldComponent(ComponentRegistry& registry,
                                        rt::PlaybackService& playback,
                                        rt::WorkspaceService& workspace,
                                        i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.id = "playback.currentTitleLabel",
       .displayName = "Current Title Label",
       .category = ComponentCategory::Playback,
       .properties = {{.name = "action",
                       .kind = PropertyKind::Enum,
                       .label = "Action",
                       .defaultValue = LayoutValue{"none"},
                       .enumValues = {"none", "reveal", "playPause", "filterByField"}}},
       .minChildren = 0,
       .optMaxChildren = 0},
      [&playback, &workspace, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      {
        return std::make_unique<NowPlayingFieldComponent>(
          playback, workspace, textCatalog, node, rt::TrackField::Title);
      });

    registry.registerComponent(
      {.id = "playback.currentArtistLabel",
       .displayName = "Current Artist Label",
       .category = ComponentCategory::Playback,
       .properties = {{.name = "action",
                       .kind = PropertyKind::Enum,
                       .label = "Action",
                       .defaultValue = LayoutValue{"none"},
                       .enumValues = {"none", "reveal", "playPause", "filterByField"}}},
       .minChildren = 0,
       .optMaxChildren = 0},
      [&playback, &workspace, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      {
        return std::make_unique<NowPlayingFieldComponent>(
          playback, workspace, textCatalog, node, rt::TrackField::Artist);
      });
  }
} // namespace ao::gtk::layout
