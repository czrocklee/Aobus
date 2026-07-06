// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "image/ImageWidget.h"
#include "image/ImageWidgetLayout.h"
#include "image/ResourceImageController.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gdkmm/cursor.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    constexpr std::int32_t kThumbnailSize = 64;

    class PassiveImageSlot final : public Gtk::Widget
    {
    public:
      PassiveImageSlot(ImageWidget& imageWidget, std::int32_t widthHint)
        : _imageWidget{imageWidget}, _widthHint{std::max(0, widthHint)}
      {
        _imageWidget.set_parent(*this);
      }

      ~PassiveImageSlot() override { _imageWidget.unparent(); }

      PassiveImageSlot(PassiveImageSlot const&) = delete;
      PassiveImageSlot& operator=(PassiveImageSlot const&) = delete;
      PassiveImageSlot(PassiveImageSlot&&) = delete;
      PassiveImageSlot& operator=(PassiveImageSlot&&) = delete;

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::CONSTANT_SIZE; }

      void measure_vfunc(Gtk::Orientation orientation,
                         int /*forSize*/,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimum = orientation == Gtk::Orientation::HORIZONTAL ? _widthHint : 0;
        natural = minimum;
        minimumBaseline = -1;
        naturalBaseline = -1;
      }

      void size_allocate_vfunc(int width, int height, int baseline) override
      {
        auto const side = _widthHint > 0 ? std::min({width, height, _widthHint}) : std::min(width, height);
        auto const offsetX = (width - side) / 2;
        auto const offsetY = (height - side) / 2;

        measureImageWidgetForSquareAllocation(_imageWidget, side);
        _imageWidget.size_allocate(Gtk::Allocation{offsetX, offsetY, side, side}, baseline);
      }

    private:
      ImageWidget& _imageWidget;
      std::int32_t _widthHint = 0;
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
        if (ctx.detail.imageCache == nullptr)
        {
          APP_LOG_ERROR("[PID {}] PlaybackImage: FAILED to create - imageCache is NULL in context!", getpid());
          _error = Gtk::make_managed<Gtk::Label>("Error: imageCache missing");
          return;
        }

        _imageWidgetPtr = std::make_unique<ImageWidget>();
        _imageControllerPtr =
          std::make_unique<ResourceImageController>(*_imageWidgetPtr, ctx.runtime.library(), *ctx.detail.imageCache);
        _imageWidgetPtr->set_overflow(Gtk::Overflow::HIDDEN);

        auto const targetSize = node.getProp<std::int64_t>("targetSize", kThumbnailSize);
        _imageWidgetPtr->setTargetSize(static_cast<std::int32_t>(targetSize));
        auto const forceSquare = node.getProp<bool>("forceSquare", false);

        if (forceSquare)
        {
          _imageWidgetPtr->setForceSquareTarget(true);
        }

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

        if (forceSquare)
        {
          _passiveSlotPtr = std::make_unique<PassiveImageSlot>(*_imageWidgetPtr, static_cast<std::int32_t>(targetSize));
          _button.set_child(*_passiveSlotPtr);
        }
        else
        {
          _button.set_child(*_imageWidgetPtr);
        }

        _button.set_has_frame(false); // Make it flat
        _button.set_overflow(Gtk::Overflow::HIDDEN);
        _button.add_css_class("ao-image-button");

        if (_action != Action::None)
        {
          _button.set_cursor(Gdk::Cursor::create("pointer"));
          _button.add_css_class("ao-clickable");
          _button.signal_clicked().connect([this] { onImageClicked(); });
        }

        _sub = _runtime.playback().onNowPlayingChanged([this](auto const&) { syncNowPlaying(); });

        _stoppedSub = _runtime.playback().onStopped(
          [this]
          {
            _currentTrackId = kInvalidTrackId;
            _currentCoverArtId = kInvalidResourceId;
            updateImage();
          });

        _idleSub = _runtime.playback().onIdle(
          [this]
          {
            _currentTrackId = kInvalidTrackId;
            _currentCoverArtId = kInvalidResourceId;
            updateImage();
          });

        syncNowPlaying();
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

      void syncNowPlaying()
      {
        auto const& nowPlaying = _runtime.playback().state().nowPlaying;

        if (nowPlaying.trackId == _currentTrackId && nowPlaying.coverArtId == _currentCoverArtId)
        {
          return;
        }

        _currentTrackId = nowPlaying.trackId;
        _currentCoverArtId = nowPlaying.coverArtId;
        updateImage();
      }

      void updateImage()
      {
        if (_currentTrackId == kInvalidTrackId)
        {
          _imageControllerPtr->clear();
          return;
        }

        if (_currentCoverArtId != kInvalidResourceId)
        {
          _imageControllerPtr->load(_currentCoverArtId);
          _button.set_visible(true);
          return;
        }

        _imageControllerPtr->clear();
        _button.set_visible(false);
      }

      rt::AppRuntime& _runtime;
      Action _action = Action::None;
      std::unique_ptr<ImageWidget> _imageWidgetPtr;
      std::unique_ptr<ResourceImageController> _imageControllerPtr;
      std::unique_ptr<PassiveImageSlot> _passiveSlotPtr;
      Gtk::Button _button;
      Gtk::Label* _error = nullptr;
      TrackId _currentTrackId = kInvalidTrackId;
      ResourceId _currentCoverArtId = kInvalidResourceId;
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
                                .category = LayoutComponentCategory::Playback,
                                .props = {{.name = "targetSize",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Target Size",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(kThumbnailSize)}},
                                          {.name = "forceSquare",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Force Square",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "action",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Action",
                                           .defaultValue = LayoutValue{"none"},
                                           .enumValues = {"none", "jumpToAlbum"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0,
                                .actionPolicy = uimodel::kExternalSecondaryActions},
                               createPlaybackImage);
  }
} // namespace ao::gtk::layout
