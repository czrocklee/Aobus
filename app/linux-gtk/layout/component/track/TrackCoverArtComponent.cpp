// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "image/ImageWidget.h"
#include "image/ImageWidgetLayout.h"
#include "image/ResourceImageController.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackDetailSizing.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class TrackCoverArtComponent final : public ILayoutComponent
    {
    public:
      class CoverArtSlot final : public Gtk::Widget
      {
      public:
        explicit CoverArtSlot(ImageWidget& imageWidget)
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
        ImageWidget& _imageWidget;
        std::int32_t _targetSize = 0;
      };

      TrackCoverArtComponent(LayoutContext& ctx, LayoutNode const& node)
        : _imageController{_imageWidget, ctx.runtime.library(), *ctx.detail.imageCache}, _slot{_imageWidget}
      {
        _imageWidget.set_halign(Gtk::Align::CENTER);
        _imageWidget.set_valign(Gtk::Align::CENTER);
        _imageWidget.set_expand(false);
        _imageWidget.set_overflow(Gtk::Overflow::HIDDEN);

        auto targetSize =
          static_cast<std::int32_t>(node.getProp<std::int64_t>("targetSize", kDefaultCoverArtTargetSize));

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

        if (ctx.track.detailScope != nullptr)
        {
          _scopeConn =
            ctx.track.detailScope->signalSnapshotChanged().connect([this](auto const& snap) { updateImage(snap); });
          updateImage(ctx.track.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override { return _slot; }

    private:
      void updateImage(rt::TrackDetailSnapshot const& snap)
      {
        if (snap.singleCoverArtId == kInvalidResourceId)
        {
          _imageController.clear();
          _imageWidget.set_visible(true);
        }
        else
        {
          _imageController.load(snap.singleCoverArtId);
          _imageWidget.set_visible(true);
        }
      }

      ImageWidget _imageWidget;
      ResourceImageController _imageController;
      CoverArtSlot _slot;
      sigc::connection _scopeConn;
    };

    std::unique_ptr<ILayoutComponent> createTrackCoverArt(LayoutContext& ctx, LayoutNode const& node)
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
                  .defaultValue = LayoutValue{true}}},
       .layoutProps = {{.name = "widthRequest", .kind = LayoutPropertyKind::Int, .label = "Width Request"},
                       {.name = "heightRequest", .kind = LayoutPropertyKind::Int, .label = "Height Request"},
                       {.name = "cssClasses", .kind = LayoutPropertyKind::String, .label = "CSS Classes"}},
       .minChildren = 0,
       .optMaxChildren = 0},
      createTrackCoverArt);
  }
} // namespace ao::gtk::layout
