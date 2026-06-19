// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/picture.h>
#include <sigc++/connection.h>

#include <cstdint>

namespace ao::gtk
{
  struct RenderTarget final
  {
    std::int32_t width;
    std::int32_t height;
  };

  RenderTarget fitSourceIntoTarget(RenderTarget source, RenderTarget target);
  bool shouldRefresh(RenderTarget current, RenderTarget next);

  class ImageWidget final : public Gtk::Picture
  {
  public:
    ImageWidget();
    ~ImageWidget() override;

    ImageWidget(ImageWidget const&) = delete;
    ImageWidget& operator=(ImageWidget const&) = delete;
    ImageWidget(ImageWidget&&) = delete;
    ImageWidget& operator=(ImageWidget&&) = delete;

    void setTargetSize(std::int32_t size);

    void setMaxRenderSize(std::int32_t width, std::int32_t height);
    void setForceSquareTarget(bool forceSquare);
    double displayScale() const;

    void setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
    void clearImage();

  protected:
    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    void invalidateRenderedImage();
    void refreshRenderedImage();
    void queueRefresh();
    void beginResizeSettle();
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
  };
} // namespace ao::gtk
