// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ResourceImageLoader.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <cstdint>

namespace ao::gtk
{
  class CoverArtView;

  class ResourceImageController final
  {
  public:
    ResourceImageController(CoverArtView& widget, ResourceImageLoader& loader);
    ~ResourceImageController() = default;

    ResourceImageController(ResourceImageController const&) = delete;
    ResourceImageController& operator=(ResourceImageController const&) = delete;
    ResourceImageController(ResourceImageController&&) = delete;
    ResourceImageController& operator=(ResourceImageController&&) = delete;

    void enableThumbnailMode(std::int32_t logicalSizePx);
    void setPlaceholderPresentation(uimodel::CoverArtPlaceholderPresentation presentation);
    void load(ResourceId resourceId);
    void clear();

  private:
    void loadFullSize(ResourceId resourceId);
    void loadThumbnail(ResourceId resourceId);
    std::int32_t thumbnailPhysicalSize() const;

    CoverArtView& _widget;
    ResourceImageLoader& _loader;

    uimodel::CoverArtPlaceholderPresentation _placeholderPresentation{};
    bool _thumbnailMode = false;
    bool _showingPlaceholder = false;
    std::int32_t _thumbnailLogicalSize = 0;
    ResourceImageLoader::Request _request;
  };
} // namespace ao::gtk
