#include "TrackListAdapter.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>

namespace
{
  // Case-insensitive substring search
  bool containsCi(const std::string& haystack, const std::string& needle)
  {
    if (needle.empty())
      return true;

    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char c1, char c2) {
      return ::std::tolower(c1) == ::std::tolower(c2);
    });
    return it != haystack.end();
  }

  bool matchesFilter(const rs::fbs::TrackT& track, const Glib::ustring& filter)
  {
    if (filter.empty())
      return true;

    auto filterStr = filter.lowercase();

    // Check artist
    if (track.meta && containsCi(track.meta->artist, filterStr))
      return true;

    // Check album
    if (track.meta && containsCi(track.meta->album, filterStr))
      return true;

    // Check title
    if (track.meta && containsCi(track.meta->title, filterStr))
      return true;

    // Check tags
    for (const auto& tag : track.custom)
    {
      if (containsCi(tag->key, filterStr))
        return true;
    }

    return false;
  }
}

TrackListAdapter::TrackListAdapter(AbstractTrackList& tracks)
  : _tracks{tracks}
  , _listModel{Gio::ListStore<TrackRow>::create()}
{
  // Attach as observer
  _tracks.attach(*this);
}

TrackListAdapter::~TrackListAdapter() { _tracks.detach(*this); }

void TrackListAdapter::setFilter(const Glib::ustring& filterText)
{
  _filterText = filterText;
  refreshFilteredView();
}

void TrackListAdapter::setExprFilter(const std::string& exprString)
{
  if (exprString.empty())
  {
    _exprFilter.reset();
  }
  else
  {
    _exprFilter = rs::expr::parse(exprString);
  }
  refreshFilteredView();
}

void TrackListAdapter::refreshFilteredView()
{
  // Store current filter
  auto filter = _filterText;

  // Clear and repopulate with filtered items
  _listModel->remove_all();

  for (std::size_t i = 0; i < _tracks.size(); ++i)
  {
    const auto& [id, track] = _tracks.at(AbstractTrackList::Index{i});

    // Check quick filter
    if (!matchesFilter(track, filter))
      continue;

    // Check expression filter if set
    if (_exprFilter.has_value())
    {
      try
      {
        auto result = rs::expr::evaluate(*_exprFilter, track);
        if (!rs::expr::toBool(result))
          continue;
      }
      catch (...)
      {
        // Expression evaluation failed, skip this track
        continue;
      }
    }

    auto row = TrackRow::create(id, track);
    _listModel->append(row);
  }
}

void TrackListAdapter::onAttached()
{
  // Initial population - iterate all tracks
  for (std::size_t i = 0; i < _tracks.size(); ++i)
  {
    const auto& [id, track] = _tracks.at(AbstractTrackList::Index{i});
    auto row = TrackRow::create(id, track);
    _listModel->append(row);
  }
}

void TrackListAdapter::onBeginInsert(TrackId, AbstractTrackList::Index)
{
  // No action needed before insert
}

void TrackListAdapter::onEndInsert(TrackId id, const rs::fbs::TrackT& track, AbstractTrackList::Index index)
{
  // If filter is active, refresh the entire view to recalculate filtered positions
  if (!_filterText.empty() || _exprFilter.has_value())
  {
    refreshFilteredView();
    return;
  }

  auto row = TrackRow::create(id, track);
  _listModel->insert(static_cast<std::uint32_t>(index), row);
}

void TrackListAdapter::onBeginUpdate(TrackId, const rs::fbs::TrackT&, AbstractTrackList::Index)
{
  // No action needed before update
}

void TrackListAdapter::onEndUpdate(TrackId id, const rs::fbs::TrackT& track, AbstractTrackList::Index index)
{
  // If filter is active, refresh the entire view to recalculate filtered positions
  if (!_filterText.empty() || _exprFilter.has_value())
  {
    refreshFilteredView();
    return;
  }

  // Update by removing old and inserting new
  auto row = TrackRow::create(id, track);
  auto uintIdx = static_cast<std::uint32_t>(index);
  if (uintIdx < _listModel->get_n_items())
  {
    _listModel->remove(uintIdx);
    _listModel->insert(uintIdx, row);
  }
}

void TrackListAdapter::onBeginRemove(TrackId, const rs::fbs::TrackT&, AbstractTrackList::Index)
{
  // No action needed before remove
}

void TrackListAdapter::onEndRemove(TrackId id, AbstractTrackList::Index index)
{
  // If filter is active, refresh the entire view to recalculate filtered positions
  if (!_filterText.empty() || _exprFilter.has_value())
  {
    refreshFilteredView();
    return;
  }

  _listModel->remove(static_cast<std::uint32_t>(index));
}

void TrackListAdapter::onBeginClear()
{
  // Clear all items
  _listModel->remove_all();
}

void TrackListAdapter::onEndClear()
{
  // Nothing to do after clear
}

void TrackListAdapter::onDetached()
{
  // Nothing to do
}
