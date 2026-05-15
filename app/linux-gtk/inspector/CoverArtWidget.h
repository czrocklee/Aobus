// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/CorePrimitives.h>
#include <runtime/ProjectionTypes.h>

#include <giomm/memoryinputstream.h>
#include <gtkmm.h>

#include <cstdint>
#include <memory>
#include <span>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::gtk
{
  class CoverArtCache;

  class CoverArtWidget final : public Gtk::Picture
  {
  public:
    CoverArtWidget(library::MusicLibrary& library, CoverArtCache& cache);
    ~CoverArtWidget() override;

    CoverArtWidget(CoverArtWidget const&) = delete;
    CoverArtWidget& operator=(CoverArtWidget const&) = delete;
    CoverArtWidget(CoverArtWidget&&) = delete;
    CoverArtWidget& operator=(CoverArtWidget&&) = delete;

    void setCoverFromBytes(std::span<std::byte const> bytes);
    void setCoverPixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
    void clearCover();

    /// Bind to a runtime detail projection for reactive cover art updates.
    void bindToDetailProjection(std::shared_ptr<rt::ITrackDetailProjection> projection);

  private:
    void onDetailSnapshot(rt::TrackDetailSnapshot const& snap);

    library::MusicLibrary& _library;
    CoverArtCache& _cache;
    std::shared_ptr<rt::ITrackDetailProjection> _detailProjection;
    rt::Subscription _detailSub;
  };
} // namespace ao::gtk
