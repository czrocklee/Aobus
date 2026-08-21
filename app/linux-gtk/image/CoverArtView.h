// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "image/ImageWidget.h"
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <cairomm/context.h>
#include <cairomm/refptr.h>
#include <gdkmm/paintable.h>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/label.h>
#include <gtkmm/overlay.h>
#include <gtkmm/picture.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::gtk
{
  class CoverArtView final : public Gtk::Overlay
  {
  public:
    CoverArtView();
    ~CoverArtView() override;

    CoverArtView(CoverArtView const&) = delete;
    CoverArtView& operator=(CoverArtView const&) = delete;
    CoverArtView(CoverArtView&&) = delete;
    CoverArtView& operator=(CoverArtView&&) = delete;

    void setTargetSize(std::int32_t size);
    void setMaxRenderSize(std::int32_t width, std::int32_t height);
    void setForceSquareTarget(bool forceSquare);
    void setHighQualityRenderer(ImageWidget::HighQualityRenderer renderer);
    void setAlternativeText(std::string_view text);
    double displayScale() const;

    void setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr);
    void showPlaceholder(uimodel::CoverArtPlaceholderPresentation presentation);
    void clearImage();

    bool showingPlaceholder() const noexcept { return _showingPlaceholder; }
    bool hasImage() const noexcept { return static_cast<bool>(_image.get_paintable()); }
    Glib::RefPtr<Gdk::Paintable const> imagePaintable() const { return _image.get_paintable(); }
    uimodel::CoverArtPlaceholderPresentation const& placeholderPresentation() const noexcept { return _presentation; }

  protected:
    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    void drawVinylDecoration(Cairo::RefPtr<Cairo::Context> const& contextPtr, std::int32_t width, std::int32_t height);
    void syncPlaceholderLayers();
    void updateMonogramSize(std::int32_t width, std::int32_t height);

    Gtk::DrawingArea _placeholderSurface;
    Gtk::Picture _glyph;
    Gtk::DrawingArea _vinylDecoration;
    Gtk::Label _monogram;
    ImageWidget _image;
    uimodel::CoverArtPlaceholderPresentation _presentation{};
    std::string _glyphResourcePath{};
    std::int32_t _targetSize = 0;
    bool _showingPlaceholder = false;
  };
} // namespace ao::gtk
