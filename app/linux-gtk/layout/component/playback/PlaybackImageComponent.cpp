// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/AccessibleLabel.h"
#include "i18n/GtkText.h"
#include "image/CoverArtView.h"
#include "image/ImageWidgetLayout.h"
#include "image/ResourceImageController.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <gdkmm/cursor.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    constexpr std::int32_t kThumbnailSize = 64;

    class PassiveImageSlot final : public Gtk::Widget
    {
    public:
      PassiveImageSlot(CoverArtView& imageWidget, std::int32_t widthHint)
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
      CoverArtView& _imageWidget;
      std::int32_t _widthHint = 0;
    };

    /**
     * @brief playback.image
     */
    class PlaybackImageComponent final : public LayoutComponent
    {
    public:
      enum class Action : std::uint8_t
      {
        None,
        JumpToAlbum
      };

      PlaybackImageComponent(rt::PlaybackService& playback,
                             rt::Library& library,
                             std::function<Result<>(TrackId)> jumpToAlbum,
                             ResourceImageLoader* imageLoader,
                             i18n::MessageCatalog const& textCatalog,
                             LayoutBuildContext const& ctx,
                             LayoutNode const& node)
        : _playback{playback}
        , _library{library}
        , _jumpToAlbum{std::move(jumpToAlbum)}
        , _tooltipSurface{ctx.surface == uimodel::LayoutSurface::Tooltip}
        , _authoredVisible{node.layoutOr<bool>("visible", true)}
      {
        if (imageLoader == nullptr)
        {
          APP_LOG_ERROR("PlaybackImage: Failed to create because imageLoader is null");
          _error = Gtk::make_managed<Gtk::Label>("Error: imageLoader missing");
          return;
        }

        _imageWidgetPtr = std::make_unique<CoverArtView>();
        _imageWidgetPtr->setAlternativeText(gtkText(textCatalog, i18n::MessageId::GtkPlaybackNowPlayingCoverArt));
        _imageControllerPtr = std::make_unique<ResourceImageController>(
          *_imageWidgetPtr, *imageLoader, [this](bool const imageAvailable) { applyImageVisibility(imageAvailable); });
        auto const defaultStyle =
          uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::NowPlaying);
        auto const styleId = node.propertyOr<std::string>(
          "placeholderStyle", std::string{uimodel::coverArtPlaceholderStyleId(defaultStyle)});
        auto const optParsedStyle = uimodel::parseCoverArtPlaceholderStyle(styleId);
        _placeholderStyle = optParsedStyle.value_or(defaultStyle);

        if (!optParsedStyle)
        {
          APP_LOG_WARN("playback.image: unknown placeholderStyle '{}'; using '{}'",
                       styleId,
                       uimodel::coverArtPlaceholderStyleId(defaultStyle));
        }

        _imageWidgetPtr->set_overflow(Gtk::Overflow::HIDDEN);

        auto const targetSize = node.propertyOr<std::int64_t>("targetSize", kThumbnailSize);
        _imageWidgetPtr->setTargetSize(static_cast<std::int32_t>(targetSize));
        auto const forceSquare = node.propertyOr<bool>("forceSquare", false);

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

        auto const actionStr = node.propertyOr<std::string>("action", "none");
        APP_LOG_DEBUG("PlaybackImage: Parsing action property, raw value: '{}'", actionStr);

        _action = [actionStr]
        {
          if (actionStr == "jumpToAlbum")
          {
            return Action::JumpToAlbum;
          }

          return Action::None;
        }();

        setAccessibleLabel(_button,
                           gtkText(textCatalog,
                                   _action == Action::JumpToAlbum ? i18n::MessageId::GtkPlaybackShowCurrentAlbum
                                                                  : i18n::MessageId::GtkPlaybackNowPlayingCoverArt));

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
        _button.set_visible(false);

        if (_action != Action::None)
        {
          _button.set_cursor(Gdk::Cursor::create("pointer"));
          _button.add_css_class("ao-clickable");
          _button.signal_clicked().connect([this] { handleImageClicked(); });
        }

        _snapshotSub =
          _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { syncSnapshot(snapshot); });
        _tracksMutatedSub = _library.changes().onChanged(
          [this](rt::LibraryChangeSet const& changeSet)
          {
            if (changeSet.libraryReset || std::ranges::contains(changeSet.tracksInserted, _currentTrackId) ||
                std::ranges::contains(changeSet.tracksDeleted, _currentTrackId) ||
                std::ranges::contains(changeSet.tracksMutated, _currentTrackId))
            {
              syncCoverArtFromLibrary();
            }
          });

        syncSnapshot(_playback.snapshot());
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(_button);
      }

      void onAuthoredPropsApplied() override
      {
        if (_imageControllerPtr != nullptr)
        {
          applyImageVisibility(_imageControllerPtr->imageAvailable());
        }
      }

    private:
      void handleImageClicked()
      {
        if (_currentTrackId == kInvalidTrackId)
        {
          APP_LOG_DEBUG("PlaybackImage: Click ignored, no current track");
          return;
        }

        APP_LOG_DEBUG("PlaybackImage: Cover clicked, action: {}", static_cast<int>(_action));

        switch (_action)
        {
          case Action::JumpToAlbum:
            if (auto const result = _jumpToAlbum(_currentTrackId); !result)
            {
              APP_LOG_ERROR("PlaybackImage: Failed to jump to album: {}", result.error().message);
            }

            break;

          case Action::None:
          default: break;
        }
      }

      void syncSnapshot(rt::PlaybackSnapshot const& snapshot)
      {
        auto const& transport = snapshot.transport;
        auto const trackId =
          transport.transport == audio::Transport::Idle ? kInvalidTrackId : transport.nowPlaying.trackId;
        auto const coverArtId =
          transport.transport == audio::Transport::Idle ? kInvalidResourceId : transport.nowPlaying.coverArtId;
        auto const candidates = std::array<std::string_view, 3>{
          transport.nowPlaying.album,
          transport.nowPlaying.artist,
          transport.nowPlaying.title,
        };
        auto identity = transport.transport == audio::Transport::Idle
                          ? uimodel::CoverArtPlaceholderIdentity{}
                          : uimodel::makeCoverArtPlaceholderIdentity(candidates);

        if (_synced && trackId == _currentTrackId && coverArtId == _currentCoverArtId && identity == _currentIdentity)
        {
          return;
        }

        _synced = true;
        _currentTrackId = trackId;
        _currentCoverArtId = coverArtId;
        _currentIdentity = std::move(identity);
        _imageControllerPtr->setPlaceholderPresentation(
          uimodel::makeCoverArtPlaceholderPresentation(_placeholderStyle, _currentIdentity));
        updateImage();
      }

      void syncCoverArtFromLibrary()
      {
        if (_currentTrackId == kInvalidTrackId)
        {
          return;
        }

        auto coverArtId = kInvalidResourceId;
        auto scope = _library.snapshot();
        coverArtId = scope.trackCoverArtId(_currentTrackId);

        if (coverArtId == _currentCoverArtId)
        {
          return;
        }

        _currentCoverArtId = coverArtId;
        updateImage();
      }

      void updateImage()
      {
        _imageControllerPtr->load(_currentCoverArtId);
        applyImageVisibility(_imageControllerPtr->imageAvailable());
      }

      /**
       * @brief Sole writer of the button's visibility, so a synchronous load and a later
       * asynchronous decode can never leave the two paths disagreeing.
       *
       * Authored visibility and image availability both have to allow the widget: the author can
       * always hide it, the persistent surface otherwise stays visible and actionable behind its
       * placeholder, and the tooltip surface is only worth revealing while a decoded image is present.
       */
      void applyImageVisibility(bool const imageAvailable)
      {
        _button.set_visible(_authoredVisible && (!_tooltipSurface || imageAvailable));
      }

      rt::PlaybackService& _playback;
      rt::Library& _library;
      std::function<Result<>(TrackId)> _jumpToAlbum;
      Action _action = Action::None;
      std::unique_ptr<CoverArtView> _imageWidgetPtr;
      std::unique_ptr<PassiveImageSlot> _passiveSlotPtr;
      Gtk::Button _button;
      // Declared after the button it writes through: its availability callback must stop
      // being reachable before the button is destroyed.
      std::unique_ptr<ResourceImageController> _imageControllerPtr;
      Gtk::Label* _error = nullptr;
      TrackId _currentTrackId = kInvalidTrackId;
      ResourceId _currentCoverArtId = kInvalidResourceId;
      bool _synced = false;
      bool _tooltipSurface = false;
      bool _authoredVisible = true;
      uimodel::CoverArtPlaceholderStyle _placeholderStyle{
        uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::NowPlaying)};
      uimodel::CoverArtPlaceholderIdentity _currentIdentity{};
      async::Subscription _snapshotSub;
      async::Subscription _tracksMutatedSub;
    };
  } // namespace

  void registerPlaybackImageComponent(ComponentRegistry& registry,
                                      rt::PlaybackService& playback,
                                      rt::Library& library,
                                      std::function<Result<>(TrackId)> jumpToAlbum,
                                      ResourceImageLoader* imageLoader,
                                      i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.id = "playback.image",
       .displayName = "Playback Cover Art",
       .category = ComponentCategory::Playback,
       .properties = {{.name = "targetSize",
                       .kind = PropertyKind::Int,
                       .label = "Target Size",
                       .defaultValue = LayoutValue{static_cast<std::int64_t>(kThumbnailSize)}},
                      {.name = "forceSquare",
                       .kind = PropertyKind::Bool,
                       .label = "Force Square",
                       .defaultValue = LayoutValue{false}},
                      {.name = "action",
                       .kind = PropertyKind::Enum,
                       .label = "Action",
                       .defaultValue = LayoutValue{"none"},
                       .enumValues = {"none", "jumpToAlbum"}},
                      {.name = "placeholderStyle",
                       .kind = PropertyKind::Enum,
                       .label = "Placeholder Style",
                       .defaultValue = LayoutValue{"equalizer"},
                       .enumValues = coverArtPlaceholderStyleIds()}},
       .minChildren = 0,
       .optMaxChildren = 0,
       .surfaces = static_cast<uimodel::LayoutSurfaceCapabilityMask>(uimodel::LayoutSurfaceCapability::Main) |
                   static_cast<uimodel::LayoutSurfaceCapabilityMask>(uimodel::LayoutSurfaceCapability::Tooltip),
       .actionSlots = actionSlotBit(ActionSlot::SecondaryClick) | actionSlotBit(ActionSlot::SecondaryLongPress)},
      [&playback, &library, jumpToAlbum = std::move(jumpToAlbum), imageLoader, textCatalog](
        LayoutBuildContext const& ctx, LayoutNode const& node)
      {
        return std::make_unique<PlaybackImageComponent>(
          playback, library, jumpToAlbum, imageLoader, textCatalog, ctx, node);
      });
  }
} // namespace ao::gtk::layout
