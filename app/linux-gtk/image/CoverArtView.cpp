// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "image/CoverArtView.h"

#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <cairomm/context.h>
#include <cairomm/refptr.h>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/overlay.h>
#include <gtkmm/picture.h>
#include <gtkmm/widget.h>
#include <pango/pango-types.h>
#include <pangomm/attributes.h>
#include <pangomm/attrlist.h>
#include <pangomm/fontdescription.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kNoteResourcePath = "/org/aobus/image/no-cover/note.svg";
    constexpr auto kVinylResourcePath = "/org/aobus/image/no-cover/vinyl.svg";
    constexpr auto kEqualizerResourcePath = "/org/aobus/image/no-cover/equalizer.svg";
    constexpr auto kSoulResourcePath = "/org/aobus/image/no-cover/soul.svg";
    constexpr auto kRegularMonogramScale = 0.58;
    constexpr auto kCompactMonogramScale = 0.46;
    constexpr auto kVinylAccentRingRadiusRatio = 114.0 / 256.0;
    constexpr auto kVinylAccentOpacity = 0.36;
    constexpr double kVinylLabelNeutralRed = 42.0 / 255.0;
    constexpr double kVinylLabelNeutralGreen = 49.0 / 255.0;
    constexpr double kVinylLabelNeutralBlue = 60.0 / 255.0;
    constexpr double kVinylLabelAccentMix = 0.3;
    constexpr auto kVinylLabelOpacity = 0.96;
    constexpr auto kSoulOpacity = 0.22;
    constexpr auto kFullCircleRadians = 2.0 * std::numbers::pi;
    constexpr std::int32_t kMinimumMonogramPixels = 10;
    constexpr std::uint16_t kPangoColorChannelScale = 257U;

    std::string_view glyphResource(uimodel::CoverArtPlaceholderStyle const style) noexcept
    {
      using Style = uimodel::CoverArtPlaceholderStyle;

      switch (style)
      {
        case Style::Note: return kNoteResourcePath;
        case Style::Vinyl: return kVinylResourcePath;
        case Style::Equalizer: return kEqualizerResourcePath;
        case Style::Soul: return kSoulResourcePath;
        case Style::Monogram: return {};
      }

      return kNoteResourcePath;
    }
  } // namespace

  CoverArtView::CoverArtView()
  {
    set_overflow(Gtk::Overflow::HIDDEN);

    _placeholderSurface.set_hexpand(true);
    _placeholderSurface.set_vexpand(true);
    set_child(_placeholderSurface);

    for (auto* const child : {static_cast<Gtk::Widget*>(&_glyph),
                              static_cast<Gtk::Widget*>(&_vinylDecoration),
                              static_cast<Gtk::Widget*>(&_monogram),
                              static_cast<Gtk::Widget*>(&_image)})
    {
      child->set_halign(Gtk::Align::FILL);
      child->set_valign(Gtk::Align::FILL);
      child->set_hexpand(true);
      child->set_vexpand(true);
      add_overlay(*child);
      set_clip_overlay(*child, true);
    }

    _glyph.set_can_shrink(true);
    _glyph.set_content_fit(Gtk::ContentFit::CONTAIN);
    _vinylDecoration.add_css_class("ao-cover-vinyl-decoration");
    _vinylDecoration.set_draw_func(
      [this](Cairo::RefPtr<Cairo::Context> const& contextPtr, int const width, int const height)
      { drawVinylDecoration(contextPtr, width, height); });
    _monogram.set_halign(Gtk::Align::CENTER);
    _monogram.set_valign(Gtk::Align::CENTER);
    _monogram.add_css_class("ao-cover-monogram");
    _image.set_content_fit(Gtk::ContentFit::CONTAIN);

    clearImage();
  }

  CoverArtView::~CoverArtView()
  {
    remove_overlay(_image);
    remove_overlay(_monogram);
    remove_overlay(_vinylDecoration);
    remove_overlay(_glyph);
    unset_child();
  }

  void CoverArtView::setTargetSize(std::int32_t const size)
  {
    _targetSize = std::max(0, size);
    _image.setTargetSize(size);

    if (_showingPlaceholder)
    {
      updateMonogramSize(_targetSize, _targetSize);
      _vinylDecoration.queue_draw();
    }
  }

  void CoverArtView::setMaxRenderSize(std::int32_t const width, std::int32_t const height)
  {
    _image.setMaxRenderSize(width, height);
  }

  void CoverArtView::setForceSquareTarget(bool const forceSquare)
  {
    _image.setForceSquareTarget(forceSquare);
  }

  double CoverArtView::displayScale() const
  {
    return _image.displayScale();
  }

  void CoverArtView::setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
  {
    if (!pixbufPtr)
    {
      clearImage();
      return;
    }

    _showingPlaceholder = false;
    _placeholderSurface.set_visible(false);
    _glyph.set_visible(false);
    _vinylDecoration.set_visible(false);
    _monogram.set_visible(false);
    _image.set_visible(true);
    _image.setImagePixbuf(pixbufPtr);
  }

  void CoverArtView::showPlaceholder(uimodel::CoverArtPlaceholderPresentation presentation)
  {
    _presentation = std::move(presentation);
    _showingPlaceholder = true;
    _image.clearImage();
    _image.set_visible(false);
    _placeholderSurface.set_visible(true);
    syncPlaceholderLayers();
  }

  void CoverArtView::clearImage()
  {
    _showingPlaceholder = false;
    _image.clearImage();
    _image.set_visible(false);
    _placeholderSurface.set_visible(false);
    _glyph.set_visible(false);
    _vinylDecoration.set_visible(false);
    _monogram.set_visible(false);
  }

  void CoverArtView::size_allocate_vfunc(int const width, int const height, int const baseline)
  {
    updateMonogramSize(width, height);
    Gtk::Overlay::size_allocate_vfunc(width, height, baseline);
  }

  void CoverArtView::drawVinylDecoration(Cairo::RefPtr<Cairo::Context> const& contextPtr,
                                         std::int32_t const width,
                                         std::int32_t const height)
  {
    if (!_showingPlaceholder || _presentation.style != uimodel::CoverArtPlaceholderStyle::Vinyl || width <= 0 ||
        height <= 0)
    {
      return;
    }

    auto const accent = _vinylDecoration.get_color();
    auto const side = static_cast<double>(std::min(width, height));
    auto const centerX = static_cast<double>(width) / 2.0;
    auto const centerY = static_cast<double>(height) / 2.0;
    auto const lineWidth = std::max(1.0, side * 1.5 / 256.0);
    contextPtr->arc(centerX, centerY, side * kVinylAccentRingRadiusRatio, 0.0, kFullCircleRadians);
    contextPtr->set_line_width(lineWidth);
    contextPtr->set_source_rgba(
      accent.get_red(), accent.get_green(), accent.get_blue(), accent.get_alpha() * kVinylAccentOpacity);
    contextPtr->stroke();

    auto const labelRadius = side / 6.0;
    auto const spindleRadius = std::max(0.75, side * 3.0 / 256.0);

    contextPtr->arc(centerX, centerY, labelRadius, 0.0, kFullCircleRadians);
    contextPtr->begin_new_sub_path();
    contextPtr->arc(centerX, centerY, spindleRadius, 0.0, kFullCircleRadians);
    contextPtr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
    contextPtr->set_source_rgba(
      (kVinylLabelNeutralRed * (1.0 - kVinylLabelAccentMix)) + (accent.get_red() * kVinylLabelAccentMix),
      (kVinylLabelNeutralGreen * (1.0 - kVinylLabelAccentMix)) + (accent.get_green() * kVinylLabelAccentMix),
      (kVinylLabelNeutralBlue * (1.0 - kVinylLabelAccentMix)) + (accent.get_blue() * kVinylLabelAccentMix),
      kVinylLabelOpacity);
    contextPtr->fill();
  }

  void CoverArtView::syncPlaceholderLayers()
  {
    using Style = uimodel::CoverArtPlaceholderStyle;

    if (_presentation.style == Style::Monogram)
    {
      _glyph.set_visible(false);
      _vinylDecoration.set_visible(false);
      _monogram.set_text(_presentation.monogram);
      _monogram.set_visible(true);
      updateMonogramSize(get_width(), get_height());
      return;
    }

    _monogram.set_visible(false);

    if (auto const resourcePath = glyphResource(_presentation.style); _glyphResourcePath != resourcePath)
    {
      _glyphResourcePath = resourcePath;
      _glyph.set_resource(_glyphResourcePath);
    }

    if (_presentation.style == Style::Vinyl)
    {
      _vinylDecoration.set_visible(true);
      _vinylDecoration.queue_draw();
    }
    else
    {
      _vinylDecoration.set_visible(false);
    }

    _glyph.set_content_fit(Gtk::ContentFit::CONTAIN);
    auto const margin = _presentation.style == Style::Note ? 4 : 0;
    _glyph.set_margin_start(margin);
    _glyph.set_margin_end(margin);
    _glyph.set_margin_top(margin);
    _glyph.set_margin_bottom(margin);
    _glyph.set_opacity(_presentation.style == Style::Soul ? kSoulOpacity : 1.0);
    _glyph.set_visible(true);
  }

  void CoverArtView::updateMonogramSize(std::int32_t const width, std::int32_t const height)
  {
    if (!_showingPlaceholder || _presentation.style != uimodel::CoverArtPlaceholderStyle::Monogram)
    {
      return;
    }

    auto const resolvedWidth = width > 0 ? width : _targetSize;
    auto const resolvedHeight = height > 0 ? height : _targetSize;
    auto const scale = _presentation.monogramSize == uimodel::CoverArtPlaceholderMonogramSize::Compact
                         ? kCompactMonogramScale
                         : kRegularMonogramScale;
    auto const pixels = std::max(
      kMinimumMonogramPixels,
      static_cast<std::int32_t>(std::round(static_cast<double>(std::min(resolvedWidth, resolvedHeight)) * scale)));
    auto attributes = Pango::AttrList{};
    auto size = Pango::Attribute::create_attr_size_absolute(pixels * PANGO_SCALE);
    auto weight = Pango::Attribute::create_attr_weight(Pango::Weight::SEMIBOLD);
    auto const& color = _presentation.monogramColor;
    auto foreground =
      Pango::Attribute::create_attr_foreground(static_cast<std::uint16_t>(color.red) * kPangoColorChannelScale,
                                               static_cast<std::uint16_t>(color.green) * kPangoColorChannelScale,
                                               static_cast<std::uint16_t>(color.blue) * kPangoColorChannelScale);
    attributes.insert(size);
    attributes.insert(weight);
    attributes.insert(foreground);
    _monogram.set_attributes(attributes);
  }
} // namespace ao::gtk
