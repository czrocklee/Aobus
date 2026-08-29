// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/TransportButton.h"
#include <ao/Contract.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <gtkmm/widget.h>

#include <memory>
#include <optional>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    uimodel::PlaybackActions& requirePlaybackActions(uimodel::PlaybackActions* playbackActions)
    {
      AO_EXPECTS(playbackActions != nullptr, "TransportButtonComponent: playback actions are not bound");

      return *playbackActions;
    }

    /**
     * @brief One transport command, as a button.
     */
    class TransportButtonComponent final : public LayoutComponent
    {
    public:
      TransportButtonComponent(rt::PlaybackService& playback,
                               uimodel::PlaybackActions* playbackActions,
                               i18n::MessageCatalog const& textCatalog,
                               LayoutNode const& node,
                               PlaybackCommand const command)
        : _button{playback,
                  requirePlaybackActions(playbackActions),
                  textCatalog,
                  command,
                  node.propertyOr<bool>("showLabel", false),
                  node.propertyOr<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };
  } // namespace

  void registerTransportButtonComponent(ComponentRegistry& registry,
                                        rt::PlaybackService& playback,
                                        uimodel::PlaybackActions* playbackActions,
                                        i18n::MessageCatalog const& textCatalog)
  {
    registry.registerSharedComponent(
      "playback.transportButton",
      {.properties =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}}},
      [&playback, playbackActions, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      {
        auto const optCommand = playbackCommandFor(node.propertyOr<std::string>(kCommandProp, ""));
        return std::make_unique<TransportButtonComponent>(
          playback, playbackActions, textCatalog, node, optCommand.value_or(PlaybackCommand::PlayPause));
      });
  }
} // namespace ao::gtk::layout
