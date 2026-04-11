// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackRow.h"

#include "model/TrackRowDataProvider.h"

#include <cstddef>
#include <cstdint>
#include <string>

TrackRow::TrackRow()
{
}

Glib::RefPtr<TrackRow> TrackRow::create(TrackId id, std::shared_ptr<app::model::TrackRowDataProvider> provider)
{
  auto obj = Glib::make_refptr_for_instance<TrackRow>(new TrackRow());
  obj->_id = id;
  obj->_provider = std::move(provider);
  return obj;
}

void TrackRow::ensureLoaded() const
{
  if (_loaded)
  {
    return;
  }

  if (!_provider)
  {
    return;
  }

  auto optRow = _provider->getRow(_id);
  if (optRow)
  {
    _artist = std::move(optRow->artist);
    _album = std::move(optRow->album);
    _title = std::move(optRow->title);
    _tags = std::move(optRow->tags);
    _resourceId = optRow->coverArtId ? std::optional<std::uint64_t>{optRow->coverArtId.value()} : std::nullopt;
  }
  _loaded = true;
}

Glib::ustring TrackRow::getArtist() const
{
  ensureLoaded();
  return Glib::Markup::escape_text(_artist);
}

Glib::ustring TrackRow::getAlbum() const
{
  ensureLoaded();
  return Glib::Markup::escape_text(_album);
}

Glib::ustring TrackRow::getTitle() const
{
  ensureLoaded();
  return Glib::Markup::escape_text(_title);
}

Glib::ustring TrackRow::getTags() const
{
  ensureLoaded();
  return Glib::Markup::escape_text(_tags);
}

std::uint64_t TrackRow::getResourceId() const
{
  ensureLoaded();
  return _resourceId.value_or(0);
}