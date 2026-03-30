// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackListAdapter.h"

#include "TrackRow.h"
#include "model/TrackRowDataProvider.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>

namespace
{
  // Case-insensitive substring search
  bool containsCi(std::string const& haystack, std::string const& needle)
  {
    if (needle.empty()) return true;

    auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](unsigned char c1, unsigned char c2) {
        return std::tolower(c1) == std::tolower(c2);
      });
    return it != haystack.end();
  }

  bool matchesFilter(std::string const& artist,
                     std::string const& album,
                     std::string const& title,
                     std::string const& tags,
                     Glib::ustring const& filter)
  {
    if (filter.empty()) return true;

    auto filterStr = filter.lowercase();

    if (containsCi(artist, filterStr)) return true;
    if (containsCi(album, filterStr)) return true;
    if (containsCi(title, filterStr)) return true;
    if (containsCi(tags, filterStr)) return true;

    return false;
  }
}

TrackListAdapter::TrackListAdapter(app::model::TrackIdList& source,
                                   std::shared_ptr<app::model::TrackRowDataProvider> provider)
  : _source{&source}, _provider{std::move(provider)}, _listModel(Gio::ListStore<TrackRow>::create())
{
  _source->attach(this);
}

TrackListAdapter::~TrackListAdapter()
{
  _source->detach(this);
}

void TrackListAdapter::setFilter(Glib::ustring const& filterText)
{
  _filterText = filterText;
  rebuildView();
}

void TrackListAdapter::createRowForTrack(TrackId id)
{
  // Load row data for filtering (quick filter needs artist/album/title/tags)
  auto const optRow = _provider->getRow(id);

  if (!optRow)
  {
    return; // Track not found or missing
  }

  auto const& rowData = *optRow;

  // Apply quick filter if set
  if (!matchesFilter(rowData.artist, rowData.album, rowData.title, rowData.tags, _filterText)) { return; }

  // Create lazy TrackRow - data loaded on demand via provider
  auto row = TrackRow::create(id, _provider);
  _listModel->append(row);
}

void TrackListAdapter::rebuildView()
{
  _listModel->remove_all();

  for (std::size_t i = 0; i < _source->size(); ++i)
  {
    auto const id = _source->trackIdAt(i);
    createRowForTrack(id);
  }
}

void TrackListAdapter::onReset()
{
  rebuildView();
}

void TrackListAdapter::onInserted(TrackId id, std::size_t index)
{
  // If filter is active, rebuild to recalculate positions
  if (!_filterText.empty())
  {
    rebuildView();
    return;
  }

  // Load and insert at position
  auto const optRow = _provider->getRow(id);

  if (!optRow) { return; }

  auto const& rowData = *optRow;
  auto row = TrackRow::create(id, _provider);

  auto const uintIdx = static_cast<std::uint32_t>(index);
  if (uintIdx <= _listModel->get_n_items()) { _listModel->insert(uintIdx, row); }
}

void TrackListAdapter::onUpdated(TrackId id, std::size_t index)
{
  // If filter is active, rebuild
  if (!_filterText.empty())
  {
    rebuildView();
    return;
  }

  // Update the row at position
  auto const optRow = _provider->getRow(id);

  auto const uintIdx = static_cast<std::uint32_t>(index);
  if (uintIdx >= _listModel->get_n_items()) { return; }

  if (!optRow)
  {
    // Track was deleted or missing - remove it
    _listModel->remove(uintIdx);
    return;
  }

  auto const& rowData = *optRow;
  auto row = TrackRow::create(id, _provider);

  _listModel->remove(uintIdx);
  _listModel->insert(uintIdx, row);
}

void TrackListAdapter::onRemoved(TrackId id, std::size_t index)
{
  // If filter is active, rebuild
  if (!_filterText.empty())
  {
    rebuildView();
    return;
  }

  auto const uintIdx = static_cast<std::uint32_t>(index);
  if (uintIdx < _listModel->get_n_items()) { _listModel->remove(uintIdx); }
}