// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <runtime/CorePrimitives.h>

#include <giomm/memoryinputstream.h>
#include <gtkmm.h>

#include <cstdint>
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

  private:
    Glib::RefPtr<Gdk::Pixbuf> resolveCover(std::vector<ao::TrackId> const& ids) const;

    ao::app::AppSession& _session;
    CoverArtCache& _cache;
    ao::app::Subscription _selSub;
  };
} // namespace ao::gtk
