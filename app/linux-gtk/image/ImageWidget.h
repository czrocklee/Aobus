// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "runtime/CorePrimitives.h"
#include "runtime/ProjectionTypes.h"

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/picture.h>

#include <cstddef>
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

    void setImageFromBytes(std::span<std::byte const> bytes);
    void setImagePixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
    void clearImage();

    void loadImage(ResourceId coverArtId);

    /// Bind to a runtime detail projection for reactive cover art updates.
    void bindToDetailProjection(std::shared_ptr<rt::ITrackDetailProjection> projection);

  private:
    void onDetailSnapshot(rt::TrackDetailSnapshot const& snap);

    library::MusicLibrary& _library;
    ImageCache& _cache;
    std::shared_ptr<rt::ITrackDetailProjection> _detailProjection;
    rt::Subscription _detailSub;
  };
} // namespace ao::gtk
