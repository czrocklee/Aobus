// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ResourceImageLoader.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <cstdint>
#include <functional>

namespace ao::gtk
{
  class CoverArtView;

  class ResourceImageController final
  {
  public:
    using ImageAvailabilityChanged = std::function<void(bool)>;

    ResourceImageController(CoverArtView& widget,
                            ResourceImageLoader& loader,
                            ImageAvailabilityChanged imageAvailabilityChanged = {});
    ~ResourceImageController() = default;

    ResourceImageController(ResourceImageController const&) = delete;
    ResourceImageController& operator=(ResourceImageController const&) = delete;
    ResourceImageController(ResourceImageController&&) = delete;
    ResourceImageController& operator=(ResourceImageController&&) = delete;

    void enableThumbnailMode(std::int32_t logicalSizePx);
    void setPlaceholderPresentation(uimodel::CoverArtPlaceholderPresentation presentation);
    void load(ResourceId resourceId);
    void clear();

    /// Level-triggered availability. The change callback only reports transitions, so observers
    /// that must stay in sync after every load have to read this instead of latching the callback.
    bool imageAvailable() const noexcept { return _imageAvailable; }

  private:
    void loadFullSize(ResourceId resourceId);
    void loadThumbnail(ResourceId resourceId);
    std::int32_t thumbnailPhysicalSize() const;
    void setImageAvailable(bool available);

    CoverArtView& _widget;
    ResourceImageLoader& _loader;

    uimodel::CoverArtPlaceholderPresentation _placeholderPresentation{};
    bool _thumbnailMode = false;
    bool _showingPlaceholder = false;
    bool _imageAvailable = false;
    std::int32_t _thumbnailLogicalSize = 0;
    ImageAvailabilityChanged _imageAvailabilityChanged;
    ResourceImageLoader::Request _request;
  };
} // namespace ao::gtk
