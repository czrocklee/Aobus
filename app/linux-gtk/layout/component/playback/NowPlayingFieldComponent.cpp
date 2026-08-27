// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/NowPlayingFieldLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
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
      NowPlayingFieldComponent(rt::AppRuntime& runtime,
                               i18n::MessageCatalog const& textCatalog,
                               LayoutNode const& node,
                               rt::TrackField const field)
        : _label{runtime,
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
                                        rt::AppRuntime& runtime,
                                        i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.type = "playback.currentTitleLabel",
       .displayName = "Current Title Label",
       .category = LayoutComponentCategory::Playback,
       .props = {{.name = "action",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Action",
                  .defaultValue = LayoutValue{"none"},
                  .enumValues = {"none", "reveal", "playPause", "filterByField"}}},
       .minChildren = 0,
       .optMaxChildren = 0},
      [&runtime, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      { return std::make_unique<NowPlayingFieldComponent>(runtime, textCatalog, node, rt::TrackField::Title); });

    registry.registerComponent(
      {.type = "playback.currentArtistLabel",
       .displayName = "Current Artist Label",
       .category = LayoutComponentCategory::Playback,
       .props = {{.name = "action",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Action",
                  .defaultValue = LayoutValue{"none"},
                  .enumValues = {"none", "reveal", "playPause", "filterByField"}}},
       .minChildren = 0,
       .optMaxChildren = 0},
      [&runtime, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      { return std::make_unique<NowPlayingFieldComponent>(runtime, textCatalog, node, rt::TrackField::Artist); });
  }
} // namespace ao::gtk::layout
