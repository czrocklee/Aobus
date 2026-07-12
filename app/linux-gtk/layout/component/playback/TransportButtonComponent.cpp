// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "playback/TransportButton.h"
#include <ao/Exception.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackSequenceService.h>
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
    uimodel::PlaybackCommandSurface& commandSurface(LayoutBuildContext& ctx)
    {
      if (ctx.dependencies.playbackCommandSurface == nullptr)
      {
        throwException<Exception>("TransportButtonComponent: playback command surface is not bound");
      }

      return *ctx.dependencies.playbackCommandSurface;
    }

    rt::PlaybackSequenceService& playbackSequence(LayoutBuildContext& ctx)
    {
      if (ctx.dependencies.playbackSequence == nullptr)
      {
        throwException<Exception>("TransportButtonComponent: playback sequence is not bound");
      }

      return *ctx.dependencies.playbackSequence;
    }

    /**
     * @brief Generic transport button component.
     */
    class TransportButtonComponent final : public LayoutComponent
    {
    public:
      TransportButtonComponent(LayoutBuildContext& ctx, LayoutNode const& node, TransportButton::Action action)
        : _button{ctx.runtime.playback(),
                  playbackSequence(ctx),
                  commandSurface(ctx),
                  action,
                  node.propertyOr<bool>("showLabel", false),
                  node.propertyOr<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };

    std::unique_ptr<LayoutComponent> createPlayPauseButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::PlayPause);
    }

    std::unique_ptr<LayoutComponent> createStopButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Stop);
    }

    std::unique_ptr<LayoutComponent> createNextButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Next);
    }

    std::unique_ptr<LayoutComponent> createPreviousButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Previous);
    }

    std::unique_ptr<LayoutComponent> createShuffleButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Shuffle);
    }

    std::unique_ptr<LayoutComponent> createRepeatButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Repeat);
    }

    std::unique_ptr<LayoutComponent> createPlayButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Play);
    }

    std::unique_ptr<LayoutComponent> createPauseButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Pause);
    }
  } // namespace

  void registerTransportButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.playPauseButton",
                                .displayName = "Play/Pause Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createPlayPauseButton);

    registry.registerComponent({.type = "playback.stopButton",
                                .displayName = "Stop Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createStopButton);

    registry.registerComponent({.type = "playback.nextButton",
                                .displayName = "Next Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createNextButton);

    registry.registerComponent({.type = "playback.previousButton",
                                .displayName = "Previous Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createPreviousButton);

    registry.registerComponent({.type = "playback.shuffleButton",
                                .displayName = "Shuffle Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createShuffleButton);

    registry.registerComponent({.type = "playback.repeatButton",
                                .displayName = "Repeat Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createRepeatButton);

    registry.registerComponent({.type = "playback.playButton",
                                .displayName = "Play Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createPlayButton);

    registry.registerComponent({.type = "playback.pauseButton",
                                .displayName = "Pause Button",
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "showLabel",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Label",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "size",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Size",
                                           .defaultValue = LayoutValue{"normal"},
                                           .enumValues = {"small", "normal", "large"}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createPauseButton);
  }
} // namespace ao::gtk::layout
