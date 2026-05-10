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

namespace ao::app
{
  class AppSession;
}

namespace ao::gtk
{
  class CoverArtCache;

  class CoverArtWidget final : public Gtk::Picture
  {
  public:
    CoverArtWidget(ao::app::AppSession& session, CoverArtCache& cache);
    ~CoverArtWidget() override;

    void setCoverFromBytes(std::span<std::byte const> bytes);
    void setCoverPixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
    void clearCover();

    /// Bind to a runtime detail projection for reactive cover art updates.
    void bindToDetailProjection(std::shared_ptr<ao::app::ITrackDetailProjection> projection);

  private:
    void onDetailSnapshot(ao::app::TrackDetailSnapshot const& snap);

    ao::app::AppSession& _session;
    CoverArtCache& _cache;
    std::shared_ptr<ao::app::ITrackDetailProjection> _detailProjection;
    ao::app::Subscription _detailSub;
  };
} // namespace ao::gtk
