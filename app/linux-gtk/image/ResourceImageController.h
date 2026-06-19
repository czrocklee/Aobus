// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ThumbnailLoader.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>

#include <cstdint>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::gtk
{
  class ImageCache;
  class ImageWidget;

  class ResourceImageController final
  {
  public:
    ResourceImageController(ImageWidget& widget, library::MusicLibrary& library, ImageCache& cache);
    ~ResourceImageController() = default;

    ResourceImageController(ResourceImageController const&) = delete;
    ResourceImageController& operator=(ResourceImageController const&) = delete;
    ResourceImageController(ResourceImageController&&) = delete;
    ResourceImageController& operator=(ResourceImageController&&) = delete;

    void enableThumbnailMode(ThumbnailLoader& loader, std::int32_t logicalSizePx);
    void load(ResourceId resourceId);
    void clear();

    void bindToDetailProjection(std::unique_ptr<rt::ITrackDetailProjection> projectionPtr);

  private:
    void onDetailSnapshot(rt::TrackDetailSnapshot const& snap);
    void loadFullSize(ResourceId resourceId);
    void loadThumbnail(ResourceId resourceId, std::uint64_t generation);
    std::int32_t thumbnailPhysicalSize() const;

    ImageWidget& _widget;
    library::MusicLibrary& _library;
    ImageCache& _cache;
    std::unique_ptr<rt::ITrackDetailProjection> _detailProjectionPtr;
    rt::Subscription _detailSub;

    bool _thumbnailMode = false;
    ThumbnailLoader* _thumbnailLoader = nullptr;
    std::int32_t _thumbnailLogicalSize = 0;
    std::uint64_t _thumbnailGeneration = 0;
    ThumbnailLoader::Request _thumbnailRequest;
  };
} // namespace ao::gtk
