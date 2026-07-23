// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ResourceImageController.h"

#include "image/ImageWidget.h"
#include "image/ResourceImageLoader.h"
#include <ao/CoreIds.h>
#include <ao/rt/projection/TrackDetailProjection.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace ao::gtk
{
  ResourceImageController::ResourceImageController(ImageWidget& widget, ResourceImageLoader& loader)
    : _widget{widget}, _loader{loader}
  {
  }

  void ResourceImageController::enableThumbnailMode(std::int32_t logicalSizePx)
  {
    _thumbnailMode = true;
    _thumbnailLogicalSize = std::max(1, logicalSizePx);
  }

  void ResourceImageController::load(ResourceId const resourceId)
  {
    _request.reset();

    if (_thumbnailMode)
    {
      loadThumbnail(resourceId);
      return;
    }

    loadFullSize(resourceId);
  }

  void ResourceImageController::clear()
  {
    _request.reset();
    _widget.clearImage();
  }

  void ResourceImageController::bindToDetailProjection(std::unique_ptr<rt::TrackDetailProjection> projectionPtr)
  {
    _detailProjectionPtr = std::move(projectionPtr);
    _detailSub = _detailProjectionPtr->subscribe(std::bind_front(&ResourceImageController::handleDetailSnapshot, this));
  }

  void ResourceImageController::handleDetailSnapshot(rt::TrackDetailSnapshot const& snap)
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

    if (auto cachedPtr = _loader.getFull(resourceId); cachedPtr)
    {
      _widget.setImagePixbuf(cachedPtr);
      return;
    }

    _widget.clearImage();
    _request = _loader.requestFull(resourceId,
                                   [this](Glib::RefPtr<Gdk::Pixbuf> const& decodedPtr)
                                   {
                                     if (!decodedPtr)
                                     {
                                       _widget.clearImage();
                                       return;
                                     }

                                     _widget.setImagePixbuf(decodedPtr);
                                   });
  }

  void ResourceImageController::loadThumbnail(ResourceId const resourceId)
  {
    if (resourceId == kInvalidResourceId)
    {
      _widget.clearImage();
      return;
    }

    auto const physicalSize = thumbnailPhysicalSize();

    if (auto cachedPtr = _loader.getThumbnail(resourceId, physicalSize); cachedPtr)
    {
      _widget.setImagePixbuf(cachedPtr);
      return;
    }

    // Cache miss: clear the recycled image immediately, then wait for the shared
    // decode to return on the UI thread.
    _widget.clearImage();

    _request = _loader.requestThumbnail(resourceId,
                                        physicalSize,
                                        [this](Glib::RefPtr<Gdk::Pixbuf> const& decodedPtr)
                                        {
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
