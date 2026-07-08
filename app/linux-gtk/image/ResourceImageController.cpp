// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ResourceImageController.h"

#include "image/ImageCache.h"
#include "image/ImageWidget.h"
#include "image/ThumbnailLoader.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/projection/TrackDetailProjection.h>

#include <gdkmm/pixbuf.h>
#include <giomm/memoryinputstream.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <utility>

namespace ao::gtk
{
  ResourceImageController::ResourceImageController(ImageWidget& widget, rt::Library const& reads, ImageCache& cache)
    : _widget{widget}, _reads{reads}, _cache{cache}
  {
  }

  void ResourceImageController::enableThumbnailMode(ThumbnailLoader& loader, std::int32_t logicalSizePx)
  {
    _thumbnailMode = true;
    _thumbnailLoader = &loader;
    _thumbnailLogicalSize = std::max(1, logicalSizePx);
  }

  void ResourceImageController::load(ResourceId const resourceId)
  {
    ++_thumbnailGeneration;
    _thumbnailRequest.reset();

    if (_thumbnailMode)
    {
      loadThumbnail(resourceId, _thumbnailGeneration);
      return;
    }

    loadFullSize(resourceId);
  }

  void ResourceImageController::clear()
  {
    ++_thumbnailGeneration;
    _thumbnailRequest.reset();
    _widget.clearImage();
  }

  void ResourceImageController::bindToDetailProjection(std::unique_ptr<rt::TrackDetailProjection> projectionPtr)
  {
    _detailProjectionPtr = std::move(projectionPtr);
    _detailSub = _detailProjectionPtr->subscribe(std::bind_front(&ResourceImageController::onDetailSnapshot, this));
  }

  void ResourceImageController::onDetailSnapshot(rt::TrackDetailSnapshot const& snap)
  {
    if (snap.selectionKind == rt::SelectionKind::None || snap.trackIds.empty())
    {
      clear();
      return;
    }

    load(snap.singleCoverArtId);
  }

  void ResourceImageController::loadFullSize(ResourceId const resourceId)
  {
    if (resourceId == kInvalidResourceId)
    {
      _widget.clearImage();
      return;
    }

    auto cachedPtr = _cache.get(ImageCacheKey::full(resourceId));

    if (!cachedPtr)
    {
      auto scope = _reads.reader();
      auto const optData = scope.loadResource(resourceId);

      if (!optData)
      {
        _widget.clearImage();
        return;
      }

      try
      {
        auto const memStreamPtr = Gio::MemoryInputStream::create();
        memStreamPtr->add_data(optData->data(), std::ssize(*optData), nullptr);
        cachedPtr = Gdk::Pixbuf::create_from_stream(memStreamPtr);
        _cache.put(ImageCacheKey::full(resourceId), cachedPtr);
      }
      catch (Glib::Error const&)
      {
        _widget.clearImage();
        return;
      }
    }

    _widget.setImagePixbuf(cachedPtr);
  }

  void ResourceImageController::loadThumbnail(ResourceId const resourceId, std::uint64_t const generation)
  {
    if (resourceId == kInvalidResourceId)
    {
      _widget.clearImage();
      return;
    }

    if (_thumbnailLoader == nullptr)
    {
      return;
    }

    auto const physicalSize = thumbnailPhysicalSize();

    if (auto cachedPtr = _thumbnailLoader->get(resourceId, physicalSize); cachedPtr)
    {
      _widget.setImagePixbuf(cachedPtr);
      return;
    }

    // Cache miss: clear the recycled image immediately, then wait for the shared
    // decode to return on the UI thread.
    _widget.clearImage();

    _thumbnailRequest = _thumbnailLoader->request(resourceId,
                                                  physicalSize,
                                                  [this, generation](Glib::RefPtr<Gdk::Pixbuf> const& decodedPtr)
                                                  {
                                                    if (generation != _thumbnailGeneration)
                                                    {
                                                      return;
                                                    }

                                                    if (!decodedPtr)
                                                    {
                                                      _widget.clearImage();
                                                      return;
                                                    }

                                                    _widget.setImagePixbuf(decodedPtr);
                                                  });
  }

  std::int32_t ResourceImageController::thumbnailPhysicalSize() const
  {
    auto const physical =
      static_cast<std::int32_t>(std::ceil(static_cast<double>(_thumbnailLogicalSize) * _widget.displayScale()));
    return std::max(1, physical);
  }
} // namespace ao::gtk
