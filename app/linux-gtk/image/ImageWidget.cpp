// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidget.h"

#include "image/ImageCache.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/ProjectionTypes.h>

#include <gdkmm/pixbuf.h>
#include <giomm/memoryinputstream.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

namespace ao::gtk
{
  ImageWidget::ImageWidget(library::MusicLibrary& library, ImageCache& cache)
    : _library{library}, _cache{cache}
  {
    set_keep_aspect_ratio(true);
    set_can_shrink(true);
    set_alternative_text("No cover art");
  }

  ImageWidget::~ImageWidget() = default;

  void ImageWidget::setTargetSize(std::int32_t size)
  {
    _targetSize = size;
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

    setImagePixbuf(cachedPtr);
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
      setImagePixbuf(Gdk::Pixbuf::create_from_stream(memStreamPtr));
    }
    catch (Glib::Error const&)
    {
      clearImage();
    }
  }

  Glib::RefPtr<Gdk::Pixbuf> ImageWidget::scalePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf) const
  {
    if (!pixbuf || _targetSize <= 0)
    {
      return pixbuf;
    }

    std::int32_t const width = pixbuf->get_width();
    std::int32_t const height = pixbuf->get_height();

    if (width <= _targetSize && height <= _targetSize)
    {
      return pixbuf; // Already small enough
    }

    double const scale = std::min(static_cast<double>(_targetSize) / static_cast<double>(width),
                                  static_cast<double>(_targetSize) / static_cast<double>(height));

    std::int32_t const newWidth = std::max(1, static_cast<std::int32_t>(static_cast<double>(width) * scale));
    std::int32_t const newHeight = std::max(1, static_cast<std::int32_t>(static_cast<double>(height) * scale));

    return pixbuf->scale_simple(newWidth, newHeight, Gdk::InterpType::BILINEAR);
  }

  void ImageWidget::setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf)
  {
    auto const scaledPtr = scalePixbuf(pixbuf);
    scaledPtr ? set_pixbuf(scaledPtr) : clearImage();
  }

  void ImageWidget::clearImage()
  {
    set_pixbuf({});
  }
}
