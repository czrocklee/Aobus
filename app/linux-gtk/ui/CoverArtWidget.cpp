// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArtWidget.h"
#include "CoverArtCache.h"
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <runtime/AppSession.h>
#include <runtime/EventTypes.h>

namespace ao::gtk
{
  CoverArtWidget::CoverArtWidget(ao::app::AppSession& session, CoverArtCache& cache)
    : _session{session}, _cache{cache}
  {
    set_keep_aspect_ratio(true);
    set_alternative_text("No cover art");

    _selSub = _session.events().subscribe<ao::app::ViewSelectionChanged>(
      [this](ao::app::ViewSelectionChanged const& ev)
      {
        if (ev.selection.empty())
        {
          clearCover();
          return;
        }
        auto const pixbuf = resolveCover(ev.selection);
        pixbuf ? setCoverPixbuf(pixbuf) : clearCover();
      });
  }

  CoverArtWidget::~CoverArtWidget() = default;

  Glib::RefPtr<Gdk::Pixbuf> CoverArtWidget::resolveCover(std::vector<ao::TrackId> const& ids) const
  {
    for (auto const trackId : ids)
    {
      auto txn = _session.musicLibrary().readTransaction();
      auto reader = _session.musicLibrary().tracks().reader(txn);
      auto const optView = reader.get(trackId, ao::library::TrackStore::Reader::LoadMode::Both);
      if (!optView)
      {
        continue;
      }

      auto const coverId = optView->metadata().coverArtId();
      if (coverId == 0)
      {
        continue;
      }

      auto const rid = static_cast<std::uint64_t>(coverId);
      auto cached = _cache.get(rid);
      if (cached)
      {
        return cached;
      }

      auto resReader = _session.musicLibrary().resources().reader(txn);
      auto const optData = resReader.get(rid);
      if (!optData)
      {
        continue;
      }

      try
      {
        auto memStream = Gio::MemoryInputStream::create();
        memStream->add_data(optData->data(), optData->size(), nullptr);
        auto pixbuf = Gdk::Pixbuf::create_from_stream(memStream);
        _cache.put(rid, pixbuf);
        return pixbuf;
      }
      catch (Glib::Error const&)
      {
        continue;
      }
    }
    return {};
  }

  void CoverArtWidget::setCoverFromBytes(std::span<std::byte const> bytes)
  {
    if (bytes.empty())
    {
      clearCover();
      return;
    }
    try
    {
      auto memStream = Gio::MemoryInputStream::create();
      memStream->add_data(bytes.data(), bytes.size(), nullptr);
      set_pixbuf(Gdk::Pixbuf::create_from_stream(memStream));
    }
    catch (Glib::Error const&)
    {
      clearCover();
    }
  }

  void CoverArtWidget::setCoverPixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf)
  {
    pixbuf ? set_pixbuf(pixbuf) : clearCover();
  }

  void CoverArtWidget::clearCover()
  {
    set_pixbuf({});
  }
}
