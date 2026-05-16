// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponents.h"
#include "app/AobusSoul.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/AobusSoulBinding.h"
#include "playback/NowPlayingFieldLabel.h"
#include "playback/OutputSelector.h"
#include "playback/SeekControl.h"
#include "playback/TimeLabel.h"
#include "playback/TransportButton.h"
#include "playback/VolumeControl.h"
#include <runtime/AppRuntime.h>

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief playback.playPauseButton
     */
    class PlayPauseButtonComponent final : public ILayoutComponent
    {
    public:
      PlayPauseButtonComponent(LayoutContext& ctx, LayoutNode const& node)
        : _button{ctx.runtime.playback(),
                  TransportButton::Action::PlayPause,
                  [&runtime = ctx.runtime] { runtime.playSelectionInFocusedView(); },
                  node.getProp<bool>("showLabel", false),
                  node.getProp<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };

    /**
     * @brief playback.stopButton
     */
    class StopButtonComponent final : public ILayoutComponent
    {
    public:
      StopButtonComponent(LayoutContext& ctx, LayoutNode const& node)
        : _button{ctx.runtime.playback(),
                  TransportButton::Action::Stop,
                  {},
                  node.getProp<bool>("showLabel", false),
                  node.getProp<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };

    /**
     * @brief playback.volumeControl
     */
    class VolumeControlComponent final : public ILayoutComponent
    {
    public:
      VolumeControlComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _control{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _control.widget(); }

    private:
      VolumeControl _control;
    };

    /**
     * @brief playback.currentTitleLabel
     */
    class CurrentTitleLabelComponent final : public ILayoutComponent
    {
    public:
      CurrentTitleLabelComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _label{ctx.runtime.playback(), NowPlayingFieldLabel::Field::Title}
      {
      }

      Gtk::Widget& widget() override { return _label.widget(); }

    private:
      NowPlayingFieldLabel _label;
    };

    /**
     * @brief playback.currentArtistLabel
     */
    class CurrentArtistLabelComponent final : public ILayoutComponent
    {
    public:
      CurrentArtistLabelComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _label{ctx.runtime.playback(), NowPlayingFieldLabel::Field::Artist}
      {
      }

      Gtk::Widget& widget() override { return _label.widget(); }

    private:
      NowPlayingFieldLabel _label;
    };

    /**
     * @brief playback.seekSlider
     */
    class SeekSliderComponent final : public ILayoutComponent
    {
    public:
      SeekSliderComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _control{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _control.widget(); }

    private:
      SeekControl _control;
    };

    /**
     * @brief playback.timeLabel
     */
    class TimeLabelComponent final : public ILayoutComponent
    {
    public:
      TimeLabelComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _label{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _label.widget(); }

    private:
      TimeLabel _label;
    };

    /**
     * @brief playback.playButton
     */
    class PlayButtonComponent final : public ILayoutComponent
    {
    public:
      PlayButtonComponent(LayoutContext& ctx, LayoutNode const& node)
        : _button{ctx.runtime.playback(),
                  TransportButton::Action::Play,
                  [&runtime = ctx.runtime] { runtime.playSelectionInFocusedView(); },
                  node.getProp<bool>("showLabel", false),
                  node.getProp<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };

    /**
     * @brief playback.pauseButton
     */
    class PauseButtonComponent final : public ILayoutComponent
    {
    public:
      PauseButtonComponent(LayoutContext& ctx, LayoutNode const& node)
        : _button{ctx.runtime.playback(),
                  TransportButton::Action::Pause,
                  {},
                  node.getProp<bool>("showLabel", false),
                  node.getProp<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };

    /**
     * @brief playback.outputButton
     */
    class OutputButtonComponent final : public ILayoutComponent
    {
    public:
      OutputButtonComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _selector{ctx.runtime.playback()}
      {
      }

      Gtk::Widget& widget() override { return _selector.widget(); }

    private:
      OutputSelector _selector;
    };

    /**
     * @brief playback.qualityIndicator
     */
    class QualityIndicatorComponent final : public ILayoutComponent
    {
    public:
      QualityIndicatorComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _runtime{ctx.runtime}, _soulBinding{std::make_unique<AobusSoulBinding>(_soul, _runtime.playback())}
      {
        _soul.set_size_request(24, 24);
        _soul.set_valign(Gtk::Align::CENTER);
        _soul.set_halign(Gtk::Align::CENTER);
      }

      Gtk::Widget& widget() override { return _soul; }

    private:
      rt::AppRuntime& _runtime;
      AobusSoul _soul{};
      std::unique_ptr<AobusSoulBinding> _soulBinding;
    };

    std::unique_ptr<ILayoutComponent> createPlayPauseButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlayPauseButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createStopButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<StopButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createVolumeControl(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<VolumeControlComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createCurrentTitleLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<CurrentTitleLabelComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createCurrentArtistLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<CurrentArtistLabelComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createSeekSlider(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SeekSliderComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createTimeLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TimeLabelComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createPlayButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlayButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createPauseButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PauseButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createOutputButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<OutputButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createQualityIndicator(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<QualityIndicatorComponent>(ctx, node);
    }
  } // namespace

  void registerPlaybackComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "playback.playPauseButton",
       .displayName = "Play/Pause Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createPlayPauseButton);

    registry.registerComponent(
      {.type = "playback.stopButton",
       .displayName = "Stop Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createStopButton);

    registry.registerComponent({.type = "playback.volumeControl",
                                .displayName = "Volume Control",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createVolumeControl);

    registry.registerComponent({.type = "playback.currentTitleLabel",
                                .displayName = "Current Title Label",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createCurrentTitleLabel);

    registry.registerComponent({.type = "playback.currentArtistLabel",
                                .displayName = "Current Artist Label",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createCurrentArtistLabel);

    registry.registerComponent({.type = "playback.seekSlider",
                                .displayName = "Seek Slider",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createSeekSlider);

    registry.registerComponent({.type = "playback.timeLabel",
                                .displayName = "Time Label",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTimeLabel);

    registry.registerComponent(
      {.type = "playback.playButton",
       .displayName = "Play Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createPlayButton);

    registry.registerComponent(
      {.type = "playback.pauseButton",
       .displayName = "Pause Button",
       .category = "Playback",
       .container = false,
       .props =
         {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label", .defaultValue = LayoutValue{false}},
          {.name = "size",
           .kind = PropertyKind::Enum,
           .label = "Size",
           .defaultValue = LayoutValue{"normal"},
           .enumValues = {"small", "normal", "large"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createPauseButton);

    registry.registerComponent({.type = "playback.outputButton",
                                .displayName = "Output Button",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createOutputButton);

    registry.registerComponent({.type = "playback.qualityIndicator",
                                .displayName = "Quality Indicator",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createQualityIndicator);
  }
} // namespace ao::gtk::layout
