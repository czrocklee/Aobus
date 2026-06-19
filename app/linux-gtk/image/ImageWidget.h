// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/picture.h>
#include <sigc++/connection.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Runtime;
  class LifetimeScope;
}

namespace ao::gtk
{
  struct RenderTarget final
  {
    std::int32_t width;
    std::int32_t height;
  };

  RenderTarget fitSourceIntoTarget(RenderTarget source, RenderTarget target);
  bool shouldRefresh(RenderTarget current, RenderTarget next);

  class ImageCache;

  class ImageWidget final : public Gtk::Picture
  {
  public:
    ImageWidget(library::MusicLibrary& library, ImageCache& cache);
    ~ImageWidget() override;

    ImageWidget(ImageWidget const&) = delete;
    ImageWidget& operator=(ImageWidget const&) = delete;
    ImageWidget(ImageWidget&&) = delete;
    ImageWidget& operator=(ImageWidget&&) = delete;

    void setTargetSize(std::int32_t size);

    void setMaxRenderSize(std::int32_t width, std::int32_t height);
    void setForceSquareTarget(bool forceSquare);

    /// Switch this widget to asynchronous thumbnail loading: cover blobs are
    /// decoded off the UI thread at roughly @p logicalSizePx (decode-at-scale)
    /// and stored in the widget's cache. Intended for list/section thumbnails
    /// where synchronous full-resolution decoding stalls scrolling. The cache
    /// passed at construction is used as the dedicated thumbnail cache.
    void enableThumbnailMode(async::Runtime& runtime, std::int32_t logicalSizePx);

    void setImageFromBytes(std::span<std::byte const> bytes);
    void setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
    void clearImage();

    void loadImage(ResourceId coverArtId);

    /// Bind to a runtime detail projection for reactive cover art updates.
    void bindToDetailProjection(std::unique_ptr<rt::ITrackDetailProjection> projectionPtr);

  protected:
    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    void onDetailSnapshot(rt::TrackDetailSnapshot const& snap);
    void invalidateRenderedImage();
    void refreshRenderedImage();
    void queueRefresh();
    RenderTarget requestedRenderTarget() const;
    double currentDisplayScale() const;

    void loadThumbnail(ResourceId coverArtId);
    void spawnThumbnailDecode(ResourceId coverArtId);
    std::int32_t thumbnailPhysicalSize() const;

    library::MusicLibrary& _library;
    ImageCache& _cache;
    std::unique_ptr<rt::ITrackDetailProjection> _detailProjectionPtr;
    rt::Subscription _detailSub;

    // Source state
    Glib::RefPtr<Gdk::Pixbuf> _sourcePixbufPtr;
    ResourceId _currentCoverId = kInvalidResourceId;
    std::int32_t _targetSize = 0;
    std::int32_t _maxRenderWidth = 0;
    std::int32_t _maxRenderHeight = 0;

    // Last rendered state (to avoid redundant resampling)
    Glib::RefPtr<Gdk::Pixbuf> _renderedSourcePixbufPtr;
    ResourceId _renderedCoverId = kInvalidResourceId;
    std::int32_t _renderedTargetPixelWidth = 0;
    std::int32_t _renderedTargetPixelHeight = 0;
    std::int32_t _renderedPixelWidth = 0;
    std::int32_t _renderedPixelHeight = 0;

    // Current allocation state
    std::int32_t _allocatedWidth = 0;
    std::int32_t _allocatedHeight = 0;
    sigc::connection _refreshConnection;
    bool _refreshQueued = false;
    bool _forceSquareTarget = false;

    // Asynchronous thumbnail mode (off-thread decode-at-scale).
    bool _thumbnailMode = false;
    async::Runtime* _asyncRuntime = nullptr;
    std::int32_t _thumbnailLogicalSize = 0;
    std::uint64_t _thumbnailGeneration = 0;
    std::unique_ptr<async::LifetimeScope> _decodeScopePtr;
  };
} // namespace ao::gtk
