// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "image/ImageWidget.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/utility/Log.h>

#include <gdkmm/cursor.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    constexpr std::int32_t kThumbnailSize = 56;

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
        if (ctx.detail.imageCache == nullptr)
        {
          APP_LOG_ERROR("[PID {}] PlaybackImage: FAILED to create - imageCache is NULL in context!", getpid());
          _error = Gtk::make_managed<Gtk::Label>("Error: imageCache missing");
          return;
        }

        _imageWidgetPtr = std::make_unique<ImageWidget>(ctx.runtime.musicLibrary(), *ctx.detail.imageCache);
        _imageWidgetPtr->set_overflow(Gtk::Overflow::HIDDEN);

        auto const targetSize = node.getProp<std::int64_t>("targetSize", kThumbnailSize);
        _imageWidgetPtr->setTargetSize(static_cast<std::int32_t>(targetSize));

        if (auto const it = node.props.find("opacity"); it != node.props.end())
        {
          if (auto const opacity = it->second.asDouble(-1.0); opacity >= 0.0)
          {
            _button.set_opacity(opacity);
          }
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

        _button.set_child(*_imageWidgetPtr);
        _button.set_has_frame(false); // Make it flat
        _button.set_overflow(Gtk::Overflow::HIDDEN);
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
          _imageWidgetPtr->clearImage();
          return;
        }

        auto const txn = _runtime.musicLibrary().readTransaction();
        auto const reader = _runtime.musicLibrary().tracks().reader(txn);
        auto const optView = reader.get(_currentTrackId, library::TrackStore::Reader::LoadMode::Both);

        if (optView)
        {
          _imageWidgetPtr->loadImage(ResourceId{optView->metadata().coverArtId()});
          _button.set_visible(true);
        }
        else
        {
          _imageWidgetPtr->clearImage();
          _button.set_visible(false);
        }
      }

      rt::AppRuntime& _runtime;
      Action _action = Action::None;
      std::unique_ptr<ImageWidget> _imageWidgetPtr;
      Gtk::Button _button;
      Gtk::Label* _error = nullptr;
      TrackId _currentTrackId = kInvalidTrackId;
      rt::Subscription _sub;
      rt::Subscription _stoppedSub;
      rt::Subscription _idleSub;
    };

    std::unique_ptr<ILayoutComponent> createPlaybackImage(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<PlaybackImageComponent>(ctx, node);
    }
  } // namespace

  void registerPlaybackImageComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "playback.image",
                                .displayName = "Playback Cover Art",
                                .category = "Playback",
                                .container = false,
                                .props = {{.name = "action",
                                           .kind = PropertyKind::Enum,
                                           .label = "Action",
                                           .defaultValue = LayoutValue{"none"},
                                           .enumValues = {"none", "jumpToAlbum"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0,
                                .actionPolicy = uimodel::layout::kExternalSecondaryActions},
                               createPlaybackImage);
  }
} // namespace ao::gtk::layout
