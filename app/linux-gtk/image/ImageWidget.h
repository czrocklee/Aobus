// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/picture.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::gtk
{
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

    void setImageFromBytes(std::span<std::byte const> bytes);
    void setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
    void clearImage();

    void loadImage(ResourceId coverArtId);

    /// Bind to a runtime detail projection for reactive cover art updates.
    void bindToDetailProjection(std::shared_ptr<rt::ITrackDetailProjection> projection);

  private:
    void onDetailSnapshot(rt::TrackDetailSnapshot const& snap);
    Glib::RefPtr<Gdk::Pixbuf> scalePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf) const;

    library::MusicLibrary& _library;
    ImageCache& _cache;
    std::int32_t _targetSize = 0;
    std::shared_ptr<rt::ITrackDetailProjection> _detailProjection;
    rt::Subscription _detailSub;
  };
} // namespace ao::gtk
