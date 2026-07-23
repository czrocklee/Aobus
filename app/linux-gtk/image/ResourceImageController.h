// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ResourceImageLoader.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/projection/TrackDetailProjection.h>

#include <cstdint>
#include <memory>

namespace ao::gtk
{
  class ImageWidget;

  class ResourceImageController final
  {
  public:
    ResourceImageController(ImageWidget& widget, ResourceImageLoader& loader);
    ~ResourceImageController() = default;

    ResourceImageController(ResourceImageController const&) = delete;
    ResourceImageController& operator=(ResourceImageController const&) = delete;
    ResourceImageController(ResourceImageController&&) = delete;
    ResourceImageController& operator=(ResourceImageController&&) = delete;

    void enableThumbnailMode(std::int32_t logicalSizePx);
    void load(ResourceId resourceId);
    void clear();

    void bindToDetailProjection(std::unique_ptr<rt::TrackDetailProjection> projectionPtr);

  private:
    void handleDetailSnapshot(rt::TrackDetailSnapshot const& snap);
    void loadFullSize(ResourceId resourceId, std::uint64_t generation);
    void loadThumbnail(ResourceId resourceId, std::uint64_t generation);
    std::int32_t thumbnailPhysicalSize() const;

    ImageWidget& _widget;
    ResourceImageLoader& _loader;
    std::unique_ptr<rt::TrackDetailProjection> _detailProjectionPtr;
    async::Subscription _detailSub;

    bool _thumbnailMode = false;
    std::int32_t _thumbnailLogicalSize = 0;
    std::uint64_t _generation = 0;
    ResourceImageLoader::Request _request;
  };
} // namespace ao::gtk
