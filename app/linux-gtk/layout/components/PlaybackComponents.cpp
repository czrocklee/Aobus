// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponents.h"

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "app/AobusSoul.h"
#include "inspector/CoverArtWidget.h"
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
#include "runtime/AppRuntime.h"

#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief Helper to get the transport button callback.
     */
    std::function<void()> getTransportCallback(rt::AppRuntime& runtime, TransportButton::Action action)
    {
      if (action == TransportButton::Action::Play || action == TransportButton::Action::PlayPause)
      {
        return [&runtime] { runtime.playSelectionInFocusedView(); };
      }

      return {};
    }

    /**
     * @brief Generic transport button component.
     */
    class TransportButtonComponent final : public ILayoutComponent
    {
    public:
      TransportButtonComponent(LayoutContext& ctx, LayoutNode const& node, TransportButton::Action action)
        : _button{ctx.runtime.playback(),
                  action,
                  getTransportCallback(ctx.runtime, action),
                  node.getProp<bool>("showLabel", false),
                  node.getProp<std::string>("size", "normal")}
      {
      }

      Gtk::Widget& widget() override { return _button.widget(); }

    private:
      TransportButton _button;
    };

    /**
     * @brief Generic now-playing field label component.
     */
    class NowPlayingFieldComponent final : public ILayoutComponent
    {
    public:
      NowPlayingFieldComponent(LayoutContext& ctx, NowPlayingFieldLabel::Field field)
        : _label{ctx.runtime.playback(), field}
      {
      }

      Gtk::Widget& widget() override { return _label.widget(); }

    private:
      NowPlayingFieldLabel _label;
    };

    /**
     * @brief playback.coverArt
     */
    class PlaybackCoverArtComponent final : public ILayoutComponent
    {
    public:
      PlaybackCoverArtComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.inspector.coverArtCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: coverArtCache missing");
          return;
        }

        _widget = std::make_unique<CoverArtWidget>(ctx.runtime.musicLibrary(), *ctx.inspector.coverArtCache);

        _sub = ctx.runtime.playback().onNowPlayingChanged(
          [this, &ctx](auto const& ev)
          {
            if (ev.trackId != _currentTrackId)
            {
              _currentTrackId = ev.trackId;
              updateCoverArt(ctx.runtime.musicLibrary());
            }
          });

        // Initial update
        _currentTrackId = ctx.runtime.playback().state().trackId;
        updateCoverArt(ctx.runtime.musicLibrary());
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(*_widget);
      }

    private:
      void updateCoverArt(library::MusicLibrary& library)
      {
        if (_currentTrackId == ao::kInvalidTrackId)
        {
          _widget->clearCover();
          return;
        }

        auto const txn = library.readTransaction();
        auto const reader = library.tracks().reader(txn);
        auto const optView = reader.get(_currentTrackId, library::TrackStore::Reader::LoadMode::Both);

        if (optView)
        {
          _widget->loadCoverArt(ResourceId{optView->metadata().coverArtId()});
        }
        else
        {
          _widget->clearCover();
        }
      }

      std::unique_ptr<CoverArtWidget> _widget;
      Gtk::Label* _error = nullptr;
      TrackId _currentTrackId = ao::kInvalidTrackId;
      rt::Subscription _sub;
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
      }

      Gtk::Widget& widget() override { return _soul; }

    private:
      rt::AppRuntime& _runtime;
      AobusSoul _soul{};
      std::unique_ptr<AobusSoulBinding> _soulBinding;
    };

    std::unique_ptr<ILayoutComponent> createPlayPauseButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::PlayPause);
    }

    std::unique_ptr<ILayoutComponent> createStopButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Stop);
    }

    std::unique_ptr<ILayoutComponent> createVolumeControl(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<VolumeControlComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createCurrentTitleLabel(LayoutContext& ctx, LayoutNode const& /*node*/)
    {
      return std::make_unique<NowPlayingFieldComponent>(ctx, NowPlayingFieldLabel::Field::Title);
    }

    std::unique_ptr<ILayoutComponent> createCurrentArtistLabel(LayoutContext& ctx, LayoutNode const& /*node*/)
    {
      return std::make_unique<NowPlayingFieldComponent>(ctx, NowPlayingFieldLabel::Field::Artist);
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
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Play);
    }

    std::unique_ptr<ILayoutComponent> createPauseButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Pause);
    }

    std::unique_ptr<ILayoutComponent> createOutputButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<OutputButtonComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createQualityIndicator(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<QualityIndicatorComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createPlaybackCoverArt(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlaybackCoverArtComponent>(ctx, node);
    }
  } // namespace

  void registerPlaybackComponents(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.coverArt",
                                .displayName = "Playback Cover Art",
                                .category = "Playback",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createPlaybackCoverArt);

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
