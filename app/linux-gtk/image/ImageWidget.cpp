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

  void ImageWidget::bindToDetailProjection(std::shared_ptr<rt::ITrackDetailProjection> projection)
  {
    _detailProjection = std::move(projection);
    _detailSub = _detailProjection->subscribe(std::bind_front(&ImageWidget::onDetailSnapshot, this));
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
    auto cached = _cache.get(rid);

    if (!cached)
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
        auto const memStream = Gio::MemoryInputStream::create();
        memStream->add_data(optData->data(), std::ssize(*optData), nullptr);
        cached = Gdk::Pixbuf::create_from_stream(memStream);
        _cache.put(rid, cached);
      }
      catch (Glib::Error const&)
      {
        clearImage();
        return;
      }
    }

    setImagePixbuf(cached);
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
      auto const memStream = Gio::MemoryInputStream::create();
      memStream->add_data(bytes.data(), std::ssize(bytes), nullptr);
      setImagePixbuf(Gdk::Pixbuf::create_from_stream(memStream));
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
    auto const scaled = scalePixbuf(pixbuf);
    scaled ? set_pixbuf(scaled) : clearImage();
  }

  void ImageWidget::clearImage()
  {
    set_pixbuf({});
  }
}
