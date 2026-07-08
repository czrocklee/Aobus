// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidget.h"

#include "image/ImageRenderPolicy.h"
#include "layout/LayoutConstants.h"

#include <gdkmm/pixbuf.h>
#include <gdkmm/surface.h> // NOLINT(misc-include-cleaner)
#include <gdkmm/texture.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/native.h>
#include <gtkmm/picture.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ao::gtk
{
  namespace
  {
    constexpr std::int32_t kMinimumRefreshDelta = 2;
    constexpr double kRefreshDeltaRatio = 0.05;

    Glib::RefPtr<Gdk::Pixbuf> centerCropToAspect(Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr, RenderTarget const target)
    {
      if (!pixbufPtr || target.width <= 0 || target.height <= 0)
      {
        return pixbufPtr;
      }

      auto const sourceWidth = pixbufPtr->get_width();
      auto const sourceHeight = pixbufPtr->get_height();

      if (sourceWidth <= 0 || sourceHeight <= 0)
      {
        return pixbufPtr;
      }

      auto const targetRatio = static_cast<double>(target.width) / static_cast<double>(target.height);
      auto const sourceRatio = static_cast<double>(sourceWidth) / static_cast<double>(sourceHeight);

      auto cropWidth = sourceWidth;
      auto cropHeight = sourceHeight;

      if (sourceRatio > targetRatio)
      {
        cropWidth = std::max(1, static_cast<std::int32_t>(std::round(static_cast<double>(sourceHeight) * targetRatio)));
      }
      else if (sourceRatio < targetRatio)
      {
        cropHeight = std::max(1, static_cast<std::int32_t>(std::round(static_cast<double>(sourceWidth) / targetRatio)));
      }

      auto const cropX = std::max(0, (sourceWidth - cropWidth) / 2);
      auto const cropY = std::max(0, (sourceHeight - cropHeight) / 2);

      return Gdk::Pixbuf::create_subpixbuf(pixbufPtr, cropX, cropY, cropWidth, cropHeight);
    }
  } // namespace

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

  ImageWidget::ImageWidget()
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
    _resizeSettleConnection.disconnect();
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

  void ImageWidget::setForceSquareTarget(bool forceSquare)
  {
    if (_forceSquareTarget != forceSquare)
    {
      _forceSquareTarget = forceSquare;
      queueRefresh();
    }
  }

  void ImageWidget::setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf)
  {
    if (!pixbuf)
    {
      clearImage();
      return;
    }

    _sourcePixbufPtr = pixbuf;
    invalidateRenderedImage();
    queueRefresh();
  }

  void ImageWidget::clearImage()
  {
    _sourcePixbufPtr.reset();

    invalidateRenderedImage();

    set_paintable({});
  }

  void ImageWidget::invalidateRenderedImage()
  {
    _renderedSourcePixbufPtr.reset();
    _renderedTargetPixelWidth = 0;
    _renderedTargetPixelHeight = 0;
    _renderedPixelWidth = 0;
    _renderedPixelHeight = 0;
    _renderedWithInterim = false;
  }

  void ImageWidget::size_allocate_vfunc(int width, int height, int baseline)
  {
    Gtk::Picture::size_allocate_vfunc(width, height, baseline);

    _allocatedWidth = std::max(0, width);
    _allocatedHeight = std::max(0, height);

    queueRefresh();
  }

  void ImageWidget::beginResizeSettle()
  {
    _resizeActive = true;
    _resizeSettleConnection.disconnect();
    _resizeSettleConnection = Glib::signal_timeout().connect(
      [this]
      {
        _resizeActive = false;

        // Re-render the now-stable frame at full quality if the last paint used
        // the cheap interim filter.
        if (_renderedWithInterim)
        {
          queueRefresh();
        }

        return false;
      },
      static_cast<std::uint32_t>(layout::kImageResizeSettleDuration.count()));
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
      if (_renderedSourcePixbufPtr)
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

    // The center crop only affects pixel content, not the fit dimensions, so we
    // can size the render target without it and defer the (allocating) crop until
    // we know a re-render is actually needed.
    auto const fitTarget =
      _forceSquareTarget
        ? target
        : fitSourceIntoTarget(
            {.width = _sourcePixbufPtr->get_width(), .height = _sourcePixbufPtr->get_height()}, target);

    if (fitTarget.width <= 0 || fitTarget.height <= 0)
    {
      return;
    }

    bool const sourceChanged = _sourcePixbufPtr != _renderedSourcePixbufPtr;
    bool const sizeChanged =
      shouldRefresh({.width = _renderedTargetPixelWidth, .height = _renderedTargetPixelHeight}, target);
    bool const renderedTooSmall = _renderedPixelWidth < fitTarget.width || _renderedPixelHeight < fitTarget.height;
    // After a resize settles, the last frame may still be the cheap interim
    // render even though nothing else changed; re-render it at full quality.
    bool const qualityUpgrade = _renderedWithInterim && !_resizeActive;

    if (!sourceChanged && !sizeChanged && !renderedTooSmall && !qualityUpgrade)
    {
      return;
    }

    // A size change on an already-rendered source means the widget is being
    // resized (e.g. a window/pane drag): resample cheaply now and re-render at
    // full quality once the size has been stable for the settle window. A fresh
    // source load (sourceChanged) or the very first paint goes straight to HYPER.
    bool const hasPriorRender = _renderedTargetPixelWidth > 0 && _renderedTargetPixelHeight > 0;

    if (sizeChanged && !sourceChanged && hasPriorRender)
    {
      beginResizeSettle();
    }

    auto const sourcePixbufPtr = _forceSquareTarget ? centerCropToAspect(_sourcePixbufPtr, target) : _sourcePixbufPtr;

    if (!sourcePixbufPtr)
    {
      return;
    }

    auto renderedPixbufPtr = Glib::RefPtr<Gdk::Pixbuf>{};
    bool scaled = false;

    if (fitTarget.width == sourcePixbufPtr->get_width() && fitTarget.height == sourcePixbufPtr->get_height())
    {
      renderedPixbufPtr = sourcePixbufPtr;
    }
    else
    {
      auto const interp = _resizeActive ? Gdk::InterpType::BILINEAR : Gdk::InterpType::HYPER;
      renderedPixbufPtr = sourcePixbufPtr->scale_simple(fitTarget.width, fitTarget.height, interp);
      scaled = true;
    }

    _renderedSourcePixbufPtr = _sourcePixbufPtr;
    _renderedTargetPixelWidth = target.width;
    _renderedTargetPixelHeight = target.height;
    _renderedPixelWidth = fitTarget.width;
    _renderedPixelHeight = fitTarget.height;
    // Only an actual downscale with the cheap filter counts as interim; a 1:1
    // blit is already full fidelity.
    _renderedWithInterim = scaled && _resizeActive;

    set_paintable(Gdk::Texture::create_for_pixbuf(renderedPixbufPtr));
  }

  RenderTarget ImageWidget::requestedRenderTarget() const
  {
    double const scale = displayScale();
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

    if (_forceSquareTarget && logicalWidth > 0 && logicalHeight > 0)
    {
      auto const side = std::min(logicalWidth, logicalHeight);
      logicalWidth = side;
      logicalHeight = side;
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

  double ImageWidget::displayScale() const
  {
    if (auto const* const native = get_native(); native != nullptr)
    {
      if (auto const surfacePtr = native->get_surface(); surfacePtr)
      {
        return std::max(1.0, surfacePtr->get_scale());
      }
    }

    return std::max(1.0, static_cast<double>(get_scale_factor()));
  }
} // namespace ao::gtk
