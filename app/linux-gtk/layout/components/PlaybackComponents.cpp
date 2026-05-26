// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponents.h"

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "ao/utility/Log.h"
#include "app/AobusSoul.h"
#include "image/ImageWidget.h"
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
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/WorkspaceService.h>

#include <gdkmm/cursor.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <unistd.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    constexpr std::int32_t kThumbnailSize = 56;

    /**
     * @brief Helper to get the transport button callback.
     */
    std::function<void()> getTransportCallback(LayoutContext& ctx, TransportButton::Action action)
    {
      if (action == TransportButton::Action::Play || action == TransportButton::Action::PlayPause)
      {
        return [&ctx] { ctx.runtime.playSelectionInFocusedView(); };
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
                  ctx.playback.sequenceController,
                  action,
                  getTransportCallback(ctx, action),
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

    /**
     * @brief playback.image
     */
    class PlaybackImageComponent final : public ILayoutComponent
    {
    public:
      enum class Action : std::uint8_t
      {
        None,
        JumpToAlbum
      };

      PlaybackImageComponent(LayoutContext& ctx, LayoutNode const& node)
        : _runtime{ctx.runtime}
      {
        if (ctx.inspector.imageCache == nullptr)
        {
          APP_LOG_ERROR("[PID {}] PlaybackImage: FAILED to create - imageCache is NULL in context!", getpid());
          _error = Gtk::make_managed<Gtk::Label>("Error: imageCache missing");
          return;
        }

        _imageWidget = std::make_unique<ImageWidget>(ctx.runtime.musicLibrary(), *ctx.inspector.imageCache);

        auto const variant = node.getProp<std::string>("variant", "default");

        if (variant == "thumbnail")
        {
          _imageWidget->setTargetSize(kThumbnailSize);
          _imageWidget->set_size_request(kThumbnailSize, kThumbnailSize);
          _imageWidget->add_css_class("ao-nowplaying-image-thumb");
        }

        auto const actionStr = node.getProp<std::string>("action", "none");
        APP_LOG_DEBUG("[PID {}] PlaybackImage: Parsing action property, raw value: '{}'", getpid(), actionStr);

        _action = [actionStr]
        {
          if (actionStr == "jumpToAlbum")
          {
            return Action::JumpToAlbum;
          }

          return Action::None;
        }();

        _button.set_child(*_imageWidget);
        _button.set_has_frame(false); // Make it flat
        _button.add_css_class("ao-image-button");

        if (_action != Action::None)
        {
          _button.set_cursor(Gdk::Cursor::create("pointer"));
          _button.add_css_class("ao-clickable");
          _button.signal_clicked().connect([this] { onImageClicked(); });
        }

        _sub = _runtime.playback().onNowPlayingChanged(
          [this](auto const& ev)
          {
            if (ev.trackId != _currentTrackId)
            {
              _currentTrackId = ev.trackId;
              updateImage();
            }
          });

        _stoppedSub = _runtime.playback().onStopped(
          [this]
          {
            _currentTrackId = kInvalidTrackId;
            updateImage();
          });

        _idleSub = _runtime.playback().onIdle(
          [this]
          {
            _currentTrackId = kInvalidTrackId;
            updateImage();
          });

        // Initial update
        _currentTrackId = _runtime.playback().state().trackId;
        updateImage();
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(_button);
      }

    private:
      void onImageClicked()
      {
        if (_currentTrackId == kInvalidTrackId)
        {
          APP_LOG_DEBUG("[PID {}] PlaybackImage: Click ignored, no current track", getpid());
          return;
        }

        APP_LOG_DEBUG("[PID {}] PlaybackImage: Cover clicked, action: {}", getpid(), static_cast<int>(_action));

        switch (_action)
        {
          case Action::JumpToAlbum: _runtime.workspace().jumpToAlbum(_currentTrackId); break;

          case Action::None:
          default: break;
        }
      }

      void updateImage()
      {
        if (_currentTrackId == kInvalidTrackId)
        {
          _imageWidget->clearImage();
          return;
        }

        auto const txn = _runtime.musicLibrary().readTransaction();
        auto const reader = _runtime.musicLibrary().tracks().reader(txn);
        auto const optView = reader.get(_currentTrackId, library::TrackStore::Reader::LoadMode::Both);

        if (optView)
        {
          _imageWidget->loadImage(ResourceId{optView->metadata().coverArtId()});
        }
        else
        {
          _imageWidget->clearImage();
        }
      }

      rt::AppRuntime& _runtime;
      Action _action = Action::None;
      std::unique_ptr<ImageWidget> _imageWidget;
      Gtk::Button _button;
      Gtk::Label* _error = nullptr;
      TrackId _currentTrackId = kInvalidTrackId;
      rt::Subscription _sub;
      rt::Subscription _stoppedSub;
      rt::Subscription _idleSub;
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
      TimeLabelComponent(LayoutContext& ctx, LayoutNode const& node)
        : _label{ctx.runtime.playback(),
                 [mode = node.getProp<std::string>("mode", "default")]
                 {
                   if (mode == "elapsed")
                   {
                     return TimeLabelMode::Elapsed;
                   }

                   if (mode == "duration")
                   {
                     return TimeLabelMode::Duration;
                   }

                   return TimeLabelMode::Default;
                 }()}
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

    std::unique_ptr<ILayoutComponent> createNextButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Next);
    }

    std::unique_ptr<ILayoutComponent> createPreviousButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Previous);
    }

    std::unique_ptr<ILayoutComponent> createShuffleButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Shuffle);
    }

    std::unique_ptr<ILayoutComponent> createRepeatButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TransportButtonComponent>(ctx, node, TransportButton::Action::Repeat);
    }

    std::unique_ptr<ILayoutComponent> createVolumeControl(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<VolumeControlComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createCurrentTitleLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<NowPlayingFieldComponent>(ctx, node, rt::TrackField::Title);
    }

    std::unique_ptr<ILayoutComponent> createCurrentArtistLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<NowPlayingFieldComponent>(ctx, node, rt::TrackField::Artist);
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

    std::unique_ptr<ILayoutComponent> createPlaybackImage(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlaybackImageComponent>(ctx, node);
    }
  } // namespace

  void registerPlaybackComponents(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.image",
                                .displayName = "Playback Cover Art",
                                .category = "Playback",
                                .container = false,
                                .props = {{.name = "variant",
                                           .kind = PropertyKind::Enum,
                                           .label = "Variant",
                                           .defaultValue = LayoutValue{"default"},
                                           .enumValues = {"default", "thumbnail"}},
                                          {.name = "action",
                                           .kind = PropertyKind::Enum,
                                           .label = "Action",
                                           .defaultValue = LayoutValue{"none"},
                                           .enumValues = {"none", "jumpToAlbum"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createPlaybackImage);

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

    registry.registerComponent(
      {.type = "playback.nextButton",
       .displayName = "Next Button",
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
      createNextButton);

    registry.registerComponent(
      {.type = "playback.previousButton",
       .displayName = "Previous Button",
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
      createPreviousButton);

    registry.registerComponent(
      {.type = "playback.shuffleButton",
       .displayName = "Shuffle Button",
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
      createShuffleButton);

    registry.registerComponent(
      {.type = "playback.repeatButton",
       .displayName = "Repeat Button",
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
      createRepeatButton);

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
                                .props = {{.name = "mode",
                                           .kind = PropertyKind::Enum,
                                           .label = "Mode",
                                           .defaultValue = LayoutValue{"default"},
                                           .enumValues = {"default", "elapsed", "duration"}}},
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
