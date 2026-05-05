// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackListProjection.h"

#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/model/FilteredTrackIdList.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace ao::app
{
  namespace
  {
    struct SortKeys final
    {
      std::uint16_t year = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t trackNumber = 0;
      std::uint32_t durationMs = 0;
      std::string titleKey{};
      std::string artistKey{};
      std::string albumKey{};
      std::string albumArtistKey{};
      std::string genreKey{};
      std::string composerKey{};
      std::string workKey{};
    };

    struct OrderEntry final
    {
      ao::TrackId trackId{};
      SortKeys keys{};
    };

    using Comparator = std::move_only_function<bool(OrderEntry const&, OrderEntry const&)>;

    constexpr std::size_t kArticleAnLen = 3;

    auto normalizeTitle(std::string_view title) -> std::string
    {
      auto result = std::string{title};
      std::ranges::transform(result, result.begin(), [](unsigned char ch) { return std::tolower(ch); });

      if (result.starts_with("the "))
      {
        return result.substr(4);
      }

      if (result.starts_with("a "))
      {
        return result.substr(2);
      }

      if (result.starts_with("an "))
      {
        return result.substr(kArticleAnLen);
      }

      return result;
    }

    auto sortFieldNeedsCold(TrackSortField field) -> bool
    {
      switch (field)
      {
        case TrackSortField::Duration:
        case TrackSortField::DiscNumber:
        case TrackSortField::TrackNumber:
        case TrackSortField::Work: return true;
        default: return false;
      }
    }

    auto computeLoadMode(std::vector<TrackSortTerm> const& sortBy) -> ao::library::TrackStore::Reader::LoadMode
    {
      if (sortBy.empty())
      {
        return ao::library::TrackStore::Reader::LoadMode::Hot;
      }

      auto needsHot = false;
      auto needsCold = false;

      for (auto const& term : sortBy)
      {
        if (sortFieldNeedsCold(term.field))
        {
          needsCold = true;
        }
        else
        {
          needsHot = true;
        }
      }

      if (needsHot && needsCold)
      {
        return ao::library::TrackStore::Reader::LoadMode::Both;
      }
      if (needsCold)
      {
        return ao::library::TrackStore::Reader::LoadMode::Cold;
      }
      return ao::library::TrackStore::Reader::LoadMode::Hot;
    }

    auto compareNumeric(auto lhsVal, auto rhsVal) -> int
    {
      if (lhsVal < rhsVal)
      {
        return -1;
      }

      if (rhsVal < lhsVal)
      {
        return 1;
      }

      return 0;
    }

    auto compareSingleField(TrackSortTerm const& term, SortKeys const& lhs, SortKeys const& rhs) -> int
    {
      switch (term.field)
      {
        case TrackSortField::Year: return compareNumeric(lhs.year, rhs.year);
        case TrackSortField::DiscNumber: return compareNumeric(lhs.discNumber, rhs.discNumber);
        case TrackSortField::TrackNumber: return compareNumeric(lhs.trackNumber, rhs.trackNumber);
        case TrackSortField::Duration: return compareNumeric(lhs.durationMs, rhs.durationMs);
        case TrackSortField::Title: return lhs.titleKey.compare(rhs.titleKey);
        case TrackSortField::Artist: return lhs.artistKey.compare(rhs.artistKey);
        case TrackSortField::Album: return lhs.albumKey.compare(rhs.albumKey);
        case TrackSortField::AlbumArtist: return lhs.albumArtistKey.compare(rhs.albumArtistKey);
        case TrackSortField::Genre: return lhs.genreKey.compare(rhs.genreKey);
        case TrackSortField::Composer: return lhs.composerKey.compare(rhs.composerKey);
        case TrackSortField::Work: return lhs.workKey.compare(rhs.workKey);
      }
      return 0;
    }

    auto buildComparator(std::vector<TrackSortTerm> sortBy) -> Comparator
    {
      if (sortBy.empty())
      {
        return {};
      }

      return [sortBy = std::move(sortBy)](OrderEntry const& lhs, OrderEntry const& rhs) -> bool
      {
        for (auto const& term : sortBy)
        {
          auto const cmp = compareSingleField(term, lhs.keys, rhs.keys);

          if (cmp != 0)
          {
            return term.ascending ? (cmp < 0) : (cmp > 0);
          }
        }
        return false;
      };
    }

    void fillSortKeys(SortKeys& keys,
                      ao::library::TrackView const& view,
                      ao::library::DictionaryStore& dict,
                      std::vector<TrackSortTerm> const& sortBy)
    {
      for (auto const& term : sortBy)
      {
        switch (term.field)
        {
          case TrackSortField::Year: keys.year = view.metadata().year(); break;
          case TrackSortField::DiscNumber: keys.discNumber = view.metadata().discNumber(); break;
          case TrackSortField::TrackNumber: keys.trackNumber = view.metadata().trackNumber(); break;
          case TrackSortField::Duration: keys.durationMs = view.property().durationMs(); break;
          case TrackSortField::Title: keys.titleKey = normalizeTitle(view.metadata().title()); break;
          case TrackSortField::Artist: keys.artistKey = normalizeTitle(dict.get(view.metadata().artistId())); break;
          case TrackSortField::Album: keys.albumKey = normalizeTitle(dict.get(view.metadata().albumId())); break;
          case TrackSortField::AlbumArtist:
            keys.albumArtistKey = normalizeTitle(dict.get(view.metadata().albumArtistId()));
            break;
          case TrackSortField::Genre: keys.genreKey = normalizeTitle(dict.get(view.metadata().genreId())); break;
          case TrackSortField::Composer:
            keys.composerKey = normalizeTitle(dict.get(view.metadata().composerId()));
            break;
          case TrackSortField::Work: keys.workKey = normalizeTitle(dict.get(view.metadata().workId())); break;
        }
      }
    }

    auto sameSortDirection(std::vector<TrackSortTerm> const& old, std::vector<TrackSortTerm> const& updated) -> bool
    {
      if (old.size() != updated.size())
      {
        return false;
      }

      for (auto i = std::size_t{0}; i < old.size(); ++i)
      {
        if (old[i].field != updated[i].field)
        {
          return false;
        }
      }

      return true;
    }
  }

  struct TrackListProjection::Impl final
  {
    ViewId viewId;
    ao::model::FilteredTrackIdList& source;
    ao::library::MusicLibrary& library;
    std::vector<TrackSortTerm> sortBy;
    Comparator comparator;
    ao::library::TrackStore::Reader::LoadMode loadMode = ao::library::TrackStore::Reader::LoadMode::Hot;
    std::vector<OrderEntry> orderIndex;
    std::unordered_map<ao::TrackId, std::size_t> positionIndex;
    std::uint64_t rev = 0;
    std::vector<std::move_only_function<void(TrackListProjectionDeltaBatch const&)>> subscribers;

    Impl(ViewId vid, ao::model::FilteredTrackIdList& src, ao::library::MusicLibrary& lib)
      : viewId{vid}, source{src}, library{lib}
    {
      rebuildOrderIndex();
    }

    void publishDelta(TrackListProjectionDeltaBatch const& batch)
    {
      for (auto& sub : subscribers)
      {
        if (sub)
        {
          sub(batch);
        }
      }
    }

    void rebuildOrderIndex()
    {
      orderIndex.clear();
      positionIndex.clear();
      orderIndex.reserve(source.size());

      auto txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      for (auto i = std::size_t{0}; i < source.size(); ++i)
      {
        auto const trackId = source.trackIdAt(i);
        auto const optView = reader.get(trackId, loadMode);

        if (!optView)
        {
          continue;
        }

        auto entry = OrderEntry{.trackId = trackId};
        fillSortKeys(entry.keys, *optView, dict, sortBy);

        orderIndex.push_back(std::move(entry));
      }

      if (comparator)
      {
        std::ranges::sort(orderIndex, std::ref(comparator));
      }

      rebuildPositionIndex();
    }

    void rebuildPositionIndex()
    {
      positionIndex.clear();
      for (auto i = std::size_t{0}; i < orderIndex.size(); ++i)
      {
        positionIndex[orderIndex[i].trackId] = i;
      }
    }

    auto findPosition(ao::TrackId trackId) const -> std::optional<std::size_t>
    {
      auto it = positionIndex.find(trackId);
      if (it == positionIndex.end())
      {
        return std::nullopt;
      }
      return it->second;
    }

    void insertEntry(ao::TrackId trackId)
    {
      auto txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto const optView = reader.get(trackId, loadMode);

      if (!optView)
      {
        return;
      }

      auto entry = OrderEntry{.trackId = trackId};
      fillSortKeys(entry.keys, *optView, dict, sortBy);

      std::size_t pos = 0;

      if (comparator)
      {
        auto it = std::ranges::lower_bound(orderIndex, entry, std::ref(comparator));
        pos = static_cast<std::size_t>(it - orderIndex.begin());
        orderIndex.insert(it, std::move(entry));
      }
      else
      {
        pos = orderIndex.size();
        orderIndex.push_back(std::move(entry));
      }

      for (auto i = pos; i < orderIndex.size(); ++i)
      {
        positionIndex[orderIndex[i].trackId] = i;
      }

      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionInsertRange{Range{.start = pos, .count = 1}}},
      });
    }

    void removeEntry(ao::TrackId trackId)
    {
      auto const optPos = findPosition(trackId);
      if (!optPos)
      {
        return;
      }

      auto const pos = *optPos;
      orderIndex.erase(orderIndex.begin() + static_cast<std::ptrdiff_t>(pos));
      positionIndex.erase(trackId);

      for (auto i = pos; i < orderIndex.size(); ++i)
      {
        positionIndex[orderIndex[i].trackId] = i;
      }

      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionRemoveRange{Range{.start = pos, .count = 1}}},
      });
    }

    void updateEntry(ao::TrackId trackId)
    {
      auto optPos = findPosition(trackId);
      if (!optPos)
      {
        return;
      }

      auto oldPos = *optPos;
      auto& oldEntry = orderIndex[oldPos];

      auto txn = library.readTransaction();
      auto reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto const optView = reader.get(trackId, loadMode);

      if (!optView)
      {
        return;
      }

      auto newKeys = SortKeys{};
      fillSortKeys(newKeys, *optView, dict, sortBy);

      if (comparator)
      {
        auto testEntry = OrderEntry{.trackId = trackId, .keys = std::move(newKeys)};

        if (comparator(oldEntry, testEntry) == comparator(testEntry, oldEntry))
        {
          oldEntry.keys = std::move(testEntry.keys);
          publishDelta(TrackListProjectionDeltaBatch{
            .revision = ++rev,
            .deltas = {ProjectionUpdateRange{Range{.start = oldPos, .count = 1}}},
          });
          return;
        }

        removeEntry(trackId);
        insertEntry(trackId);
        return;
      }

      oldEntry.keys = std::move(newKeys);
      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionUpdateRange{Range{.start = oldPos, .count = 1}}},
      });
    }
  };

  TrackListProjection::TrackListProjection(ViewId viewId,
                                           ao::model::FilteredTrackIdList& source,
                                           ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(viewId, source, library)}
  {
    source.attach(this);
  }

  TrackListProjection::~TrackListProjection()
  {
    _impl->source.detach(this);
  }

  ViewId TrackListProjection::viewId() const noexcept
  {
    return _impl->viewId;
  }
  std::uint64_t TrackListProjection::revision() const noexcept
  {
    return _impl->rev;
  }

  void TrackListProjection::setSortBy(std::vector<TrackSortTerm> sortBy)
  {
    if (sameSortDirection(_impl->sortBy, sortBy) && _impl->comparator)
    {
      _impl->sortBy = std::move(sortBy);
      _impl->comparator = buildComparator(_impl->sortBy);
      std::ranges::reverse(_impl->orderIndex);
      _impl->rebuildPositionIndex();

      _impl->publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++_impl->rev,
        .deltas = {ProjectionReset{}},
      });
      return;
    }

    _impl->sortBy = std::move(sortBy);
    _impl->comparator = buildComparator(_impl->sortBy);
    _impl->loadMode = computeLoadMode(_impl->sortBy);
    _impl->rebuildOrderIndex();

    _impl->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_impl->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  std::size_t TrackListProjection::size() const noexcept
  {
    return _impl->orderIndex.size();
  }

  ao::TrackId TrackListProjection::trackIdAt(std::size_t index) const
  {
    if (index >= _impl->orderIndex.size())
    {
      return ao::TrackId{};
    }
    return _impl->orderIndex[index].trackId;
  }

  Subscription TrackListProjection::subscribe(
    std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler)
  {
    handler(TrackListProjectionDeltaBatch{
      .revision = _impl->rev,
      .deltas = {ProjectionReset{}},
    });

    auto index = _impl->subscribers.size();
    _impl->subscribers.push_back(std::move(handler));

    return Subscription{[this, index] { _impl->subscribers[index] = {}; }};
  }

  void TrackListProjection::onReset()
  {
    _impl->rebuildOrderIndex();
    _impl->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_impl->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  void TrackListProjection::onInserted(ao::TrackId id, std::size_t /*sourceIndex*/)
  {
    _impl->insertEntry(id);
  }

  void TrackListProjection::onUpdated(ao::TrackId id, std::size_t /*sourceIndex*/)
  {
    _impl->updateEntry(id);
  }

  void TrackListProjection::onRemoved(ao::TrackId id, std::size_t /*sourceIndex*/)
  {
    _impl->removeEntry(id);
  }

  void TrackListProjection::onInserted(std::span<ao::TrackId const> /*ids*/)
  {
    _impl->rebuildOrderIndex();
    _impl->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_impl->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  void TrackListProjection::onUpdated(std::span<ao::TrackId const> /*ids*/)
  {
    _impl->rebuildOrderIndex();
    _impl->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_impl->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  void TrackListProjection::onRemoved(std::span<ao::TrackId const> /*ids*/)
  {
    _impl->rebuildOrderIndex();
    _impl->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_impl->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  void TrackListProjection::publishDelta(TrackListProjectionDeltaBatch const& batch)
  {
    _impl->publishDelta(batch);
  }
}
