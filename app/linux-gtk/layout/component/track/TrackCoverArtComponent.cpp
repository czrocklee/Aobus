// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "app/GtkUiDependencies.h"
#include "image/CoverArtView.h"
#include "image/ImageWidgetLayout.h"
#include "image/ResourceImageController.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackDetailSizing.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/CoreIds.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class TrackCoverArtComponent final : public LayoutComponent
    {
    public:
      class CoverArtSlot final : public Gtk::Widget
      {
      public:
        explicit CoverArtSlot(CoverArtView& imageWidget)
          : _imageWidget{imageWidget}
        {
          set_overflow(Gtk::Overflow::HIDDEN);
          _imageWidget.set_parent(*this);
        }

        ~CoverArtSlot() override { _imageWidget.unparent(); }

        CoverArtSlot(CoverArtSlot const&) = delete;
        CoverArtSlot& operator=(CoverArtSlot const&) = delete;
        CoverArtSlot(CoverArtSlot&&) = delete;
        CoverArtSlot& operator=(CoverArtSlot&&) = delete;

        void setTargetSize(std::int32_t targetSize)
        {
          targetSize = std::max(0, targetSize);

          if (_targetSize == targetSize)
          {
            return;
          }

          _targetSize = targetSize;
          _imageWidget.setTargetSize(_targetSize);
          _imageWidget.setMaxRenderSize(_targetSize, _targetSize);
          queue_resize();
        }

      protected:
        Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

        void measure_vfunc(Gtk::Orientation orientation,
                           int forSize,
                           int& minimum,
                           int& natural,
                           int& minimumBaseline,
                           int& naturalBaseline) const override
        {
          minimumBaseline = -1;
          naturalBaseline = -1;

          if (orientation == Gtk::Orientation::HORIZONTAL)
          {
            minimum = 0;
            natural = forSize > 0 ? coverArtSideForWidth(forSize, _targetSize) : _targetSize;
            return;
          }

          if (forSize < 0)
          {
            minimum = 0;
            natural = _targetSize;
            return;
          }

          auto const side = coverArtSideForWidth(forSize, _targetSize);
          minimum = 0;
          natural = side;
        }

        void size_allocate_vfunc(int width, int height, int baseline) override
        {
          auto side = coverArtSideForWidth(width, _targetSize);

          if (height > 0)
          {
            side = std::min(side, height);
          }

          auto const childX = std::max(0, (width - side) / 2);
          auto const childY = std::max(0, (height - side) / 2);
          measureImageWidgetForSquareAllocation(_imageWidget, side);
          _imageWidget.size_allocate(Gtk::Allocation{childX, childY, side, side}, baseline);
        }

      private:
        CoverArtView& _imageWidget;
        std::int32_t _targetSize = 0;
      };

      TrackCoverArtComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _slot{_imageWidget}
      {
        if (ctx.dependencies.imageLoader == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: imageLoader missing");
          return;
        }

        _imageControllerPtr = std::make_unique<ResourceImageController>(_imageWidget, *ctx.dependencies.imageLoader);
        auto const defaultStyle = uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::Inspector);
        auto const styleId = node.propertyOr<std::string>(
          "placeholderStyle", std::string{uimodel::coverArtPlaceholderStyleId(defaultStyle)});
        auto const optParsedStyle = uimodel::parseCoverArtPlaceholderStyle(styleId);
        _placeholderStyle = optParsedStyle.value_or(defaultStyle);

        if (!optParsedStyle)
        {
          APP_LOG_WARN("track.coverArt: unknown placeholderStyle '{}'; using '{}'",
                       styleId,
                       uimodel::coverArtPlaceholderStyleId(defaultStyle));
        }

        _imageWidget.set_halign(Gtk::Align::FILL);
        _imageWidget.set_valign(Gtk::Align::FILL);
        _imageWidget.set_hexpand(true);
        _imageWidget.set_vexpand(true);
        _imageWidget.set_overflow(Gtk::Overflow::HIDDEN);

        auto targetSize =
          static_cast<std::int32_t>(node.propertyOr<std::int64_t>("targetSize", kDefaultCoverArtTargetSize));

        if (auto const it = node.layout.find("widthRequest"); it != node.layout.end())
        {
          targetSize = static_cast<std::int32_t>(it->second.asInt());
        }

        if (auto const it = node.layout.find("heightRequest"); it != node.layout.end())
        {
          auto const height = static_cast<std::int32_t>(it->second.asInt());
          targetSize = targetSize > 0 ? std::min(targetSize, height) : height;
        }

        _slot.setTargetSize(targetSize);

        if (auto const it = node.layout.find("cssClasses"); it != node.layout.end())
        {
          if (auto const* const classes = it->second.getIf<std::vector<std::string>>(); classes != nullptr)
          {
            for (auto const& className : *classes)
            {
              _imageWidget.add_css_class(className);
            }
          }
          else if (auto const className = it->second.asString(); !className.empty())
          {
            _imageWidget.add_css_class(className);
          }
        }

        if (ctx.detailScope != nullptr)
        {
          _scopeConn =
            ctx.detailScope->signalSnapshotChanged().connect([this](auto const& snap) { updateImage(snap); });
          updateImage(ctx.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override
      {
        if (_error != nullptr)
        {
          return *_error;
        }

        return _slot;
      }

    private:
      void updateImage(rt::TrackDetailSnapshot const& snap)
      {
        if (snap.selectionKind == rt::SelectionKind::None)
        {
          _imageControllerPtr->clear();
          _imageWidget.set_visible(false);
          return;
        }

        auto const album = uimodel::formatTrackFieldDisplayText(rt::TrackField::Album, snap, "", false);
        auto const albumArtist = uimodel::formatTrackFieldDisplayText(rt::TrackField::AlbumArtist, snap, "", false);
        auto const artist = uimodel::formatTrackFieldDisplayText(rt::TrackField::Artist, snap, "", false);
        auto const title = uimodel::formatTrackFieldDisplayText(rt::TrackField::Title, snap, "", false);
        auto const candidates = std::array<std::string_view, 4>{album, albumArtist, artist, title};
        _imageControllerPtr->setPlaceholderPresentation(uimodel::makeCoverArtPlaceholderPresentation(
          _placeholderStyle, uimodel::makeCoverArtPlaceholderIdentity(candidates)));
        _imageControllerPtr->load(snap.singleCoverArtId);
        _imageWidget.set_visible(true);
      }

      CoverArtView _imageWidget;
      std::unique_ptr<ResourceImageController> _imageControllerPtr;
      CoverArtSlot _slot;
      Gtk::Label* _error = nullptr;
      uimodel::CoverArtPlaceholderStyle _placeholderStyle{
        uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::Inspector)};
      sigc::connection _scopeConn;
    };

    std::unique_ptr<LayoutComponent> createTrackCoverArt(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackCoverArtComponent>(ctx, node);
    }
  } // namespace

  void registerTrackCoverArtComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.coverArt",
       .displayName = "Cover Art",
       .category = LayoutComponentCategory::Track,
       .props = {{.name = "targetSize",
                  .kind = LayoutPropertyKind::Int,
                  .label = "Target Size",
                  .defaultValue = LayoutValue{static_cast<std::int64_t>(kDefaultCoverArtTargetSize)}},
                 {.name = "forceSquare",
                  .kind = LayoutPropertyKind::Bool,
                  .label = "Force Square",
                  .defaultValue = LayoutValue{true}},
                 {.name = "placeholderStyle",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Placeholder Style",
                  .defaultValue = LayoutValue{"vinyl"},
                  .enumValues = coverArtPlaceholderStyleIds()}},
       .layoutProps = {{.name = "widthRequest", .kind = LayoutPropertyKind::Int, .label = "Width Request"},
                       {.name = "heightRequest", .kind = LayoutPropertyKind::Int, .label = "Height Request"},
                       {.name = "cssClasses", .kind = LayoutPropertyKind::String, .label = "CSS Classes"}},
       .minChildren = 0,
       .optMaxChildren = 0},
      createTrackCoverArt);
  }
} // namespace ao::gtk::layout
