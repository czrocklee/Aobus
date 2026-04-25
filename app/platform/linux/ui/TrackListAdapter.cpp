// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/TrackListAdapter.h"

#include "platform/linux/ui/TrackRow.h"
#include "platform/linux/ui/TrackRowDataProvider.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace app::ui
{

  TrackListAdapter::TrackListAdapter(app::core::model::TrackIdList& source, TrackRowDataProvider const& provider)
    : _source{&source}, _provider{&provider}, _listModel(Gio::ListStore<TrackRow>::create())
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
    if (auto row = _provider->getTrackRow(id))
    {
      _listModel->append(row);
    }
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

    if (auto row = _provider->getTrackRow(id))
    {
      auto const uintIdx = static_cast<std::uint32_t>(index);

      if (uintIdx <= _listModel->get_n_items())
      {
        _listModel->insert(uintIdx, row);
      }
    }
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
    auto row = _provider->getTrackRow(id);

    auto const uintIdx = static_cast<std::uint32_t>(index);

    if (uintIdx >= _listModel->get_n_items())
    {
      return;
    }

    if (!row)
    {
      // Track was deleted or missing - remove it
      _listModel->remove(uintIdx);
      return;
    }

    std::vector<Glib::RefPtr<TrackRow>> additions;
    additions.push_back(row);
    _listModel->splice(uintIdx, 1, additions);
  }

  void TrackListAdapter::onRemoved([[maybe_unused]] TrackId id, std::size_t index)
  {
    // If filter is active, rebuild

    if (!_filterText.empty())
    {
      rebuildView();
      return;
    }

    auto const uintIdx = static_cast<std::uint32_t>(index);

    if (uintIdx < _listModel->get_n_items())
    {
      _listModel->remove(uintIdx);
    }
  }

} // namespace app::ui
