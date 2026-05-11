// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArtWidget.h"
#include "CoverArtCache.h"
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>

namespace ao::gtk
{
  CoverArtWidget::CoverArtWidget(ao::app::AppSession& session, CoverArtCache& cache)
    : _session{session}, _cache{cache}
  {
    set_keep_aspect_ratio(true);
    set_alternative_text("No cover art");
  }

  CoverArtWidget::~CoverArtWidget() = default;

  void CoverArtWidget::bindToDetailProjection(std::shared_ptr<ao::app::ITrackDetailProjection> projection)
  {
    _detailProjection = std::move(projection);
    _detailSub =
      _detailProjection->subscribe([this](ao::app::TrackDetailSnapshot const& snap) { onDetailSnapshot(snap); });
  }

  void CoverArtWidget::onDetailSnapshot(ao::app::TrackDetailSnapshot const& snap)
  {
    if (snap.selectionKind == ao::app::SelectionKind::None || snap.trackIds.empty() ||
        snap.singleCoverArtId == ao::ResourceId{0})
    {
      clearCover();
      return;
    }

    auto const rid = static_cast<std::uint64_t>(snap.singleCoverArtId.value());
    auto cached = _cache.get(rid);

    if (!cached)
    {
      auto const txn = _session.musicLibrary().readTransaction();
      auto const resReader = _session.musicLibrary().resources().reader(txn);
      auto const optData = resReader.get(rid);

      if (!optData)
      {
        clearCover();
        return;
      }

      try
      {
        auto const memStream = Gio::MemoryInputStream::create();
        memStream->add_data(optData->data(), static_cast<gssize>(optData->size()), nullptr);
        cached = Gdk::Pixbuf::create_from_stream(memStream);
        _cache.put(rid, cached);
      }
      catch (Glib::Error const&)
      {
        clearCover();
        return;
      }
    }

    setCoverPixbuf(cached);
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
      auto const memStream = Gio::MemoryInputStream::create();
      memStream->add_data(bytes.data(), static_cast<gssize>(bytes.size()), nullptr);
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
