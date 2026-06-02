// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidget.h"

#include "image/ImageCache.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/ProjectionTypes.h>

#include <gdkmm/pixbuf.h>
#include <gdkmm/surface.h> // NOLINT(misc-include-cleaner)
#include <gdkmm/texture.h>
#include <giomm/memoryinputstream.h>
#include <glibmm/error.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/native.h>
#include <gtkmm/picture.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

namespace ao::gtk
{
  namespace detail
  {
    constexpr std::int32_t kMinimumRefreshDelta = 2;
    constexpr double kRefreshDeltaRatio = 0.05;

    RenderTarget fitSourceIntoTarget(RenderTarget const source, RenderTarget const target)
    {
      if (source.width <= 0 || source.height <= 0 || target.width <= 0 || target.height <= 0)
      {
        return {.width = 0, .height = 0};
      }

      // Never upscale beyond the source image if the source is smaller than the requested target.
      if (source.width <= target.width && source.height <= target.height)
      {
        return source;
      }

      double const scale = std::min(static_cast<double>(target.width) / static_cast<double>(source.width),
                                    static_cast<double>(target.height) / static_cast<double>(source.height));

      return {.width = std::max(1, static_cast<std::int32_t>(std::round(static_cast<double>(source.width) * scale))),
              .height = std::max(1, static_cast<std::int32_t>(std::round(static_cast<double>(source.height) * scale)))};
    }

    bool shouldRefresh(RenderTarget const current, RenderTarget const next)
    {
      if (current.width <= 0 || current.height <= 0)
      {
        return next.width > 0 && next.height > 0;
      }

      auto const widthDiff = std::abs(current.width - next.width);
      auto const heightDiff = std::abs(current.height - next.height);

      auto const widthThreshold =
        std::max(kMinimumRefreshDelta,
                 static_cast<std::int32_t>(std::ceil(static_cast<double>(current.width) * kRefreshDeltaRatio)));
      auto const heightThreshold =
        std::max(kMinimumRefreshDelta,
                 static_cast<std::int32_t>(std::ceil(static_cast<double>(current.height) * kRefreshDeltaRatio)));

      return widthDiff >= widthThreshold || heightDiff >= heightThreshold;
    }
  } // namespace detail

  ImageWidget::ImageWidget(library::MusicLibrary& library, ImageCache& cache)
    : _library{library}, _cache{cache}
  {
    set_content_fit(Gtk::ContentFit::CONTAIN);
    set_can_shrink(true);
    set_alternative_text("No cover art");

    signal_map().connect(
      [this]
      {
        invalidateRenderedImage();
        queueRefresh();
      });
    property_scale_factor().signal_changed().connect(
      [this]
      {
        invalidateRenderedImage();
        queueRefresh();
      });
  }

  ImageWidget::~ImageWidget()
  {
    _refreshConnection.disconnect();
  }

  void ImageWidget::setTargetSize(std::int32_t size)
  {
    if (_targetSize != size)
    {
      _targetSize = size;
      queueRefresh();
    }
  }

  void ImageWidget::setMaxRenderSize(std::int32_t width, std::int32_t height)
  {
    if (_maxRenderWidth != width || _maxRenderHeight != height)
    {
      _maxRenderWidth = width;
      _maxRenderHeight = height;
      queueRefresh();
    }
  }

  void ImageWidget::bindToDetailProjection(std::unique_ptr<rt::ITrackDetailProjection> projectionPtr)
  {
    _detailProjectionPtr = std::move(projectionPtr);
    _detailSub = _detailProjectionPtr->subscribe(std::bind_front(&ImageWidget::onDetailSnapshot, this));
  }

  void ImageWidget::onDetailSnapshot(rt::TrackDetailSnapshot const& snap)
  {
    if (snap.selectionKind == rt::SelectionKind::None || snap.trackIds.empty())
    {
      clearImage();
      return;
    }

    loadImage(snap.singleCoverArtId);
  }

  void ImageWidget::loadImage(ResourceId const coverArtId)
  {
    if (coverArtId == kInvalidResourceId)
    {
      clearImage();
      return;
    }

    auto const rid = static_cast<std::uint64_t>(coverArtId.raw());
    auto cachedPtr = _cache.get(rid);

    if (!cachedPtr)
    {
      auto const txn = _library.readTransaction();
      auto const resReader = _library.resources().reader(txn);
      auto const optData = resReader.get(rid);

      if (!optData)
      {
        clearImage();
        return;
      }

      try
      {
        auto const memStreamPtr = Gio::MemoryInputStream::create();
        memStreamPtr->add_data(optData->data(), std::ssize(*optData), nullptr);
        cachedPtr = Gdk::Pixbuf::create_from_stream(memStreamPtr);
        _cache.put(rid, cachedPtr);
      }
      catch (Glib::Error const&)
      {
        clearImage();
        return;
      }
    }

    _currentCoverId = coverArtId;
    _sourcePixbufPtr = cachedPtr;
    invalidateRenderedImage();
    queueRefresh();
  }

  void ImageWidget::setImageFromBytes(std::span<std::byte const> bytes)
  {
    if (bytes.empty())
    {
      clearImage();
      return;
    }

    try
    {
      auto const memStreamPtr = Gio::MemoryInputStream::create();
      memStreamPtr->add_data(bytes.data(), std::ssize(bytes), nullptr);
      _currentCoverId = kInvalidResourceId;
      _sourcePixbufPtr = Gdk::Pixbuf::create_from_stream(memStreamPtr);
      invalidateRenderedImage();
      queueRefresh();
    }
    catch (Glib::Error const&)
    {
      clearImage();
    }
  }

  void ImageWidget::setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf)
  {
    if (!pixbuf)
    {
      clearImage();
      return;
    }

    _currentCoverId = kInvalidResourceId;
    _sourcePixbufPtr = pixbuf;
    invalidateRenderedImage();
    queueRefresh();
  }

  void ImageWidget::clearImage()
  {
    _sourcePixbufPtr.reset();
    _currentCoverId = kInvalidResourceId;

    invalidateRenderedImage();

    set_paintable({});
  }

  void ImageWidget::invalidateRenderedImage()
  {
    _renderedSourcePixbufPtr.reset();
    _renderedCoverId = kInvalidResourceId;
    _renderedTargetPixelWidth = 0;
    _renderedTargetPixelHeight = 0;
    _renderedPixelWidth = 0;
    _renderedPixelHeight = 0;
  }

  void ImageWidget::size_allocate_vfunc(int width, int height, int baseline)
  {
    Gtk::Picture::size_allocate_vfunc(width, height, baseline);

    _allocatedWidth = std::max(0, width);
    _allocatedHeight = std::max(0, height);

    queueRefresh();
  }

  void ImageWidget::queueRefresh()
  {
    if (_refreshQueued)
    {
      return;
    }

    _refreshQueued = true;
    _refreshConnection = Glib::signal_idle().connect(
      [this]
      {
        _refreshQueued = false;
        refreshRenderedImage();
        return false;
      });
  }

  void ImageWidget::refreshRenderedImage()
  {
    if (!_sourcePixbufPtr)
    {
      if (_renderedSourcePixbufPtr || _renderedCoverId != kInvalidResourceId)
      {
        clearImage();
      }

      return;
    }

    auto const target = requestedRenderTarget();

    if (target.width <= 0 || target.height <= 0)
    {
      return;
    }

    auto const fitTarget = detail::fitSourceIntoTarget(
      {.width = _sourcePixbufPtr->get_width(), .height = _sourcePixbufPtr->get_height()}, target);

    if (fitTarget.width <= 0 || fitTarget.height <= 0)
    {
      return;
    }

    bool const sourceChanged = (_sourcePixbufPtr != _renderedSourcePixbufPtr) || (_currentCoverId != _renderedCoverId);
    bool const sizeChanged =
      detail::shouldRefresh({.width = _renderedTargetPixelWidth, .height = _renderedTargetPixelHeight}, target);
    bool const renderedTooSmall = _renderedPixelWidth < fitTarget.width || _renderedPixelHeight < fitTarget.height;

    if (!sourceChanged && !sizeChanged && !renderedTooSmall)
    {
      return;
    }

    auto renderedPixbufPtr = Glib::RefPtr<Gdk::Pixbuf>{};

    if (fitTarget.width == _sourcePixbufPtr->get_width() && fitTarget.height == _sourcePixbufPtr->get_height())
    {
      renderedPixbufPtr = _sourcePixbufPtr;
    }
    else
    {
      renderedPixbufPtr = _sourcePixbufPtr->scale_simple(fitTarget.width, fitTarget.height, Gdk::InterpType::HYPER);
    }

    _renderedSourcePixbufPtr = _sourcePixbufPtr;
    _renderedCoverId = _currentCoverId;
    _renderedTargetPixelWidth = target.width;
    _renderedTargetPixelHeight = target.height;
    _renderedPixelWidth = fitTarget.width;
    _renderedPixelHeight = fitTarget.height;

    set_paintable(Gdk::Texture::create_for_pixbuf(renderedPixbufPtr));
  }

  ImageWidget::RenderTarget ImageWidget::requestedRenderTarget() const
  {
    double const scale = currentDisplayScale();
    std::int32_t logicalWidth = 0;
    std::int32_t logicalHeight = 0;

    auto const widgetWidth = _allocatedWidth > 0 ? _allocatedWidth : get_width();
    auto const widgetHeight = _allocatedHeight > 0 ? _allocatedHeight : get_height();

    // 1. Prefer actual widget size if available.
    if (widgetWidth > 0 && widgetHeight > 0)
    {
      logicalWidth = widgetWidth;
      logicalHeight = widgetHeight;
    }
    // 2. Fall back to targetSize hint (useful before first allocation).
    else if (_targetSize > 0)
    {
      logicalWidth = _targetSize;
      logicalHeight = _targetSize;
    }
    // 3. Last resort: use source dimensions (not recommended for large images, but better than 0).
    else if (_sourcePixbufPtr)
    {
      logicalWidth = _sourcePixbufPtr->get_width();
      logicalHeight = _sourcePixbufPtr->get_height();
    }

    // Clamp to max render size when set.
    if (_maxRenderWidth > 0 && logicalWidth > _maxRenderWidth)
    {
      logicalWidth = _maxRenderWidth;
    }

    if (_maxRenderHeight > 0 && logicalHeight > _maxRenderHeight)
    {
      logicalHeight = _maxRenderHeight;
    }

    // Ensure we are rounding UP to the nearest physical pixel to maintain sharpness.
    return {.width = std::max(static_cast<std::int32_t>(1),
                              static_cast<std::int32_t>(std::ceil(static_cast<double>(logicalWidth) * scale))),
            .height = std::max(static_cast<std::int32_t>(1),
                               static_cast<std::int32_t>(std::ceil(static_cast<double>(logicalHeight) * scale)))};
  }

  double ImageWidget::currentDisplayScale() const
  {
    if (auto const* const nativePtr = get_native(); nativePtr != nullptr)
    {
      if (auto const surfacePtr = nativePtr->get_surface(); surfacePtr)
      {
        return std::max(1.0, surfacePtr->get_scale());
      }
    }

    return std::max(1.0, static_cast<double>(get_scale_factor()));
  }
} // namespace ao::gtk
