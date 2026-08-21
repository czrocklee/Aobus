// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/GtkUiDependencies.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/TransportButton.h"
#include <ao/Contract.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
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
    uimodel::PlaybackCommandSurface& commandSurface(LayoutBuildContext& ctx)
    {
      AO_EXPECTS(ctx.dependencies.playbackCommandSurface != nullptr,
                 "TransportButtonComponent: playback command surface is not bound");

      return *ctx.dependencies.playbackCommandSurface;
    }

    /**
     * @brief One transport command, as a button.
     */
    class TransportButtonComponent final : public LayoutComponent
    {
    public:
      TransportButtonComponent(LayoutBuildContext& ctx, LayoutNode const& node, PlaybackCommand const command)
        : _button{ctx.runtime.playback(),
                  commandSurface(ctx),
                  ctx.dependencies.textCatalog,
                  command,
                  node.propertyOr<bool>("showLabel", false),
                  node.propertyOr<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };

    std::unique_ptr<LayoutComponent> createTransportButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      auto const optCommand = playbackCommandFor(node.propertyOr<std::string>(kCommandProp, ""));
      return std::make_unique<TransportButtonComponent>(ctx, node, optCommand.value_or(PlaybackCommand::PlayPause));
    }
  } // namespace

  void registerTransportButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      withShellProperties(sharedComponentDescriptor(SharedLayoutComponentType::PlaybackTransportButton),
                          {{.name = "showLabel",
                            .kind = LayoutPropertyKind::Bool,
                            .label = "Show Label",
                            .defaultValue = LayoutValue{false}},
                           {.name = "size",
                            .kind = LayoutPropertyKind::Enum,
                            .label = "Size",
                            .defaultValue = LayoutValue{"normal"},
                            .enumValues = {"small", "normal", "large"}}}),
      createTransportButton);
  }
} // namespace ao::gtk::layout
