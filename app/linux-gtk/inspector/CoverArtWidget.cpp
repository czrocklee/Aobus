// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "inspector/CoverArtWidget.h"
#include "inspector/CoverArtCache.h"
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <runtime/ProjectionTypes.h>

#include <gdkmm/pixbuf.h>
#include <giomm/memoryinputstream.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

namespace ao::gtk
{
  CoverArtWidget::CoverArtWidget(library::MusicLibrary& library, CoverArtCache& cache)
    : _library{library}, _cache{cache}
  {
    set_keep_aspect_ratio(true);
    set_alternative_text("No cover art");
  }

  CoverArtWidget::~CoverArtWidget() = default;

  void CoverArtWidget::bindToDetailProjection(std::shared_ptr<rt::ITrackDetailProjection> projection)
  {
    _detailProjection = std::move(projection);
    _detailSub = _detailProjection->subscribe(std::bind_front(&CoverArtWidget::onDetailSnapshot, this));
  }

  void CoverArtWidget::onDetailSnapshot(rt::TrackDetailSnapshot const& snap)
  {
    if (snap.selectionKind == rt::SelectionKind::None || snap.trackIds.empty() ||
        snap.singleCoverArtId == ResourceId{0})
    {
      clearCover();
      return;
    }

    auto const rid = static_cast<std::uint64_t>(snap.singleCoverArtId.value());
    auto cached = _cache.get(rid);

    if (!cached)
    {
      auto const txn = _library.readTransaction();
      auto const resReader = _library.resources().reader(txn);
      auto const optData = resReader.get(rid);

      if (!optData)
      {
        clearCover();
        return;
      }

      try
      {
        auto const memStream = Gio::MemoryInputStream::create();
        memStream->add_data(optData->data(), std::ssize(*optData), nullptr);
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
      memStream->add_data(bytes.data(), std::ssize(bytes), nullptr);
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
