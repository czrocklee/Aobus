// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
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
    CoverArtWidget(ao::library::MusicLibrary& library, CoverArtCache& cache);
    ~CoverArtWidget() override;

    void setCoverFromBytes(std::span<std::byte const> bytes);
    void setCoverPixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
    void clearCover();

    /// Bind to a runtime detail projection for reactive cover art updates.
    void bindToDetailProjection(std::shared_ptr<ao::rt::ITrackDetailProjection> projection);

  private:
    void onDetailSnapshot(ao::rt::TrackDetailSnapshot const& snap);

    ao::library::MusicLibrary& _library;
    CoverArtCache& _cache;
    std::shared_ptr<ao::rt::ITrackDetailProjection> _detailProjection;
    ao::rt::Subscription _detailSub;
  };
} // namespace ao::gtk
