// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ResourceImageController.h"

#include "image/CoverArtView.h"
#include "image/ResourceImageLoader.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ao::gtk
{
  ResourceImageController::ResourceImageController(CoverArtView& widget,
                                                   ResourceImageLoader& loader,
                                                   ImageAvailabilityChanged imageAvailabilityChanged)
    : _widget{widget}
    , _loader{loader}
    , _placeholderPresentation{uimodel::makeCoverArtPlaceholderPresentation(uimodel::CoverArtPlaceholderStyle::Note,
                                                                            {})}
    , _imageAvailabilityChanged{std::move(imageAvailabilityChanged)}
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

    if (resourceId == kInvalidResourceId)
    {
      _showingPlaceholder = true;
      _widget.showPlaceholder(_placeholderPresentation);
      setImageAvailable(false);
      return;
    }

    _showingPlaceholder = false;

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
    _showingPlaceholder = false;
    _widget.clearImage();
    setImageAvailable(false);
  }

  void ResourceImageController::setPlaceholderPresentation(uimodel::CoverArtPlaceholderPresentation presentation)
  {
    _placeholderPresentation = std::move(presentation);

    if (_showingPlaceholder)
    {
      _widget.showPlaceholder(_placeholderPresentation);
    }
  }

  void ResourceImageController::loadFullSize(ResourceId const resourceId)
  {
    if (auto cachedPtr = _loader.getFull(resourceId); cachedPtr)
    {
      _widget.setImagePixbuf(cachedPtr);
      setImageAvailable(true);
      return;
    }

    _widget.clearImage();
    setImageAvailable(false);
    _request = _loader.requestFull(resourceId,
                                   [this](Glib::RefPtr<Gdk::Pixbuf> const& decodedPtr)
                                   {
                                     if (!decodedPtr)
                                     {
                                       _widget.clearImage();
                                       setImageAvailable(false);
                                       return;
                                     }

                                     _widget.setImagePixbuf(decodedPtr);
                                     setImageAvailable(true);
                                   });
  }

  void ResourceImageController::loadThumbnail(ResourceId const resourceId)
  {
    auto const physicalSize = thumbnailPhysicalSize();

    if (auto cachedPtr = _loader.getThumbnail(resourceId, physicalSize); cachedPtr)
    {
      _widget.setImagePixbuf(cachedPtr);
      setImageAvailable(true);
      return;
    }

    // Cache miss: clear the recycled image immediately, then wait for the shared
    // decode to return on the UI thread.
    _widget.clearImage();
    setImageAvailable(false);

    _request = _loader.requestThumbnail(resourceId,
                                        physicalSize,
                                        [this](Glib::RefPtr<Gdk::Pixbuf> const& decodedPtr)
                                        {
                                          if (!decodedPtr)
                                          {
                                            _widget.clearImage();
                                            setImageAvailable(false);
                                            return;
                                          }

                                          _widget.setImagePixbuf(decodedPtr);
                                          setImageAvailable(true);
                                        });
  }

  void ResourceImageController::setImageAvailable(bool const available)
  {
    if (_imageAvailable == available)
    {
      return;
    }

    _imageAvailable = available;

    if (_imageAvailabilityChanged)
    {
      _imageAvailabilityChanged(available);
    }
  }

  std::int32_t ResourceImageController::thumbnailPhysicalSize() const
  {
    auto const physical =
      static_cast<std::int32_t>(std::ceil(static_cast<double>(_thumbnailLogicalSize) * _widget.displayScale()));
    return std::max(1, physical);
  }
} // namespace ao::gtk
