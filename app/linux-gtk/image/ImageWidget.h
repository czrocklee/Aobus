// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/MainContextCallbackScope.h"
#include "image/ImageRenderPolicy.h"
#include <ao/utility/ScopedRegistration.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/picture.h>
#include <sigc++/connection.h>

#include <cstdint>
#include <functional>

namespace ao::gtk
{
  class ImageWidget final : public Gtk::Picture
  {
  public:
    // The renderer owns only immutable Pixbuf input. It must deliver completion
    // on this widget's GTK main context, and resetting the returned registration
    // must prevent any not-yet-entered owner callback from being delivered.
    using RenderedImageReady = std::function<void(Glib::RefPtr<Gdk::Pixbuf>)>;
    using HighQualityRenderer =
      std::function<utility::ScopedRegistration(Glib::RefPtr<Gdk::Pixbuf>, RenderTarget, RenderedImageReady)>;

    ImageWidget();
    ~ImageWidget() override;

    ImageWidget(ImageWidget const&) = delete;
    ImageWidget& operator=(ImageWidget const&) = delete;
    ImageWidget(ImageWidget&&) = delete;
    ImageWidget& operator=(ImageWidget&&) = delete;

    void setTargetSize(std::int32_t size);

    void setMaxRenderSize(std::int32_t width, std::int32_t height);
    void setForceSquareTarget(bool forceSquare);
    void setHighQualityRenderer(HighQualityRenderer renderer);
    double displayScale() const;

    void setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr);
    void clearImage();

  protected:
    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    void invalidateRenderedImage();
    void refreshRenderedImage();
    void queueRefresh();
    void beginResizeSettle();
    void cancelPendingRender();
    bool pendingRenderMatches(Glib::RefPtr<Gdk::Pixbuf> const& sourcePixbufPtr,
                              RenderTarget target,
                              RenderTarget renderedSize) const;
    void requestHighQualityRender(Glib::RefPtr<Gdk::Pixbuf> sourcePixbufPtr,
                                  Glib::RefPtr<Gdk::Pixbuf> renderedSourcePixbufPtr,
                                  RenderTarget target,
                                  RenderTarget renderedSize);
    void publishRenderedImage(Glib::RefPtr<Gdk::Pixbuf> const& renderedSourcePixbufPtr,
                              Glib::RefPtr<Gdk::Pixbuf> const& renderedPixbufPtr,
                              RenderTarget target,
                              RenderTarget renderedSize,
                              bool interim);
    RenderTarget requestedRenderTarget() const;

    // Source state
    Glib::RefPtr<Gdk::Pixbuf> _sourcePixbufPtr;
    std::int32_t _targetSize = 0;
    std::int32_t _maxRenderWidth = 0;
    std::int32_t _maxRenderHeight = 0;

    // Last rendered state (to avoid redundant resampling)
    Glib::RefPtr<Gdk::Pixbuf> _renderedSourcePixbufPtr;
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

    // Resize quality debounce: while allocations keep arriving we resample with a
    // cheap filter, then re-render the final frame at full quality once the size
    // has been stable for a short settle window.
    sigc::connection _resizeSettleConnection;
    bool _resizeActive = false;
    bool _renderedWithInterim = false;

    // Full-quality scaling may run off-thread, but every request and completion
    // remains owned by this GTK-main-context widget. The callback scope rejects
    // a completion retained past widget teardown; the generation rejects a
    // completion superseded by a source or target change.
    HighQualityRenderer _highQualityRenderer;
    MainContextCallbackScope _renderCallbacks;
    utility::ScopedRegistration _renderRequest;
    Glib::RefPtr<Gdk::Pixbuf> _pendingSourcePixbufPtr;
    std::int32_t _pendingTargetPixelWidth = 0;
    std::int32_t _pendingTargetPixelHeight = 0;
    std::int32_t _pendingRenderedPixelWidth = 0;
    std::int32_t _pendingRenderedPixelHeight = 0;
    std::uint64_t _renderGeneration = 0;
    bool _renderPending = false;
  };
} // namespace ao::gtk
