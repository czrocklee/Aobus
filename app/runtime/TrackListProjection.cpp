// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackListProjection.h"

#include "SmartListSource.h"
#include "TrackListPresentation.h"
#include "TrackSource.h"
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/utility/ScopedTimer.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct SortKeys final
    {
      std::uint16_t year = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t trackNumber = 0;
      std::uint32_t durationMs = 0;
      std::string_view titleKey{};
      std::string_view artistKey{};
      std::string_view albumKey{};
      std::string_view albumArtistKey{};
      std::string_view genreKey{};
      std::string_view composerKey{};
      std::string_view workKey{};
    };

    struct GroupSection final
    {
      Range rows{};
      std::string_view label{};
    };

    struct OrderEntry final
    {
      ao::TrackId trackId{};
      SortKeys keys{};
      std::string_view groupKey{};
      std::string_view groupLabel{};
    };

    using Comparator = std::move_only_function<bool(OrderEntry const&, OrderEntry const&)>;

    constexpr std::size_t kArticleAnLen = 3;

    bool iequals_prefix(std::string_view str, std::string_view prefix)
    {
      if (str.size() < prefix.size())
      {
        return false;
      }

      for (std::size_t i = 0; i < prefix.size(); ++i)
      {
        if (std::tolower(static_cast<unsigned char>(str[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        {
          return false;
        }
      }

      return true;
    }

    std::string normalizeTitle(std::string_view title)
    {
      auto offset = 0UZ;

      if (iequals_prefix(title, "the "))
      {
        offset = 4;
      }
      else if (iequals_prefix(title, "a "))
      {
        offset = 2;
      }
      else if (iequals_prefix(title, "an "))
      {
        offset = kArticleAnLen;
      }

      auto result = std::string{title.substr(offset)};
      std::ranges::transform(
        result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      return result;
    }

    bool sortFieldNeedsCold(TrackSortField field)
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

    bool groupByNeedsCold(TrackGroupKey groupBy)
    {
      return groupBy == TrackGroupKey::Work;
    }

    ao::library::TrackStore::Reader::LoadMode computeLoadMode(std::vector<TrackSortTerm> const& sortBy,
                                                              TrackGroupKey groupBy)
    {
      bool needsHot = false;
      bool needsCold = groupByNeedsCold(groupBy);

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

      if (sortBy.empty() && !needsCold)
      {
        return ao::library::TrackStore::Reader::LoadMode::Hot;
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

    int compareNumeric(auto lhsVal, auto rhsVal)
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

    int compareSingleField(TrackSortTerm const& term, SortKeys const& lhs, SortKeys const& rhs)
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

    Comparator buildComparator(std::vector<TrackSortTerm> sortBy)
    {
      if (sortBy.empty())
      {
        return {};
      }

      return [sortBy = std::move(sortBy)](OrderEntry const& lhs, OrderEntry const& rhs) -> bool
      {
        for (auto const& term : sortBy)
        {
          int const cmp = compareSingleField(term, lhs.keys, rhs.keys);

          if (cmp != 0)
          {
            return term.ascending ? (cmp < 0) : (cmp > 0);
          }
        }

        return lhs.trackId < rhs.trackId;
      };
    }

    std::string_view intern(std::unordered_set<std::string>& pool, std::string&& str)
    {
      if (str.empty())
      {
        return {};
      }

      return *pool.insert(std::move(str)).first;
    }

    void fillSortKeys(SortKeys& keys,
                      ao::library::TrackView const& view,
                      ao::library::DictionaryStore& dict,
                      std::vector<TrackSortTerm> const& sortBy,
                      std::unordered_map<ao::DictionaryId, std::string>& normCache,
                      std::unordered_set<std::string>& stringPool)
    {
      auto const getNorm = [&](ao::DictionaryId id) -> std::string_view
      {
        if (auto it = normCache.find(id); it != normCache.end())
        {
          return it->second;
        }

        return normCache.emplace(id, normalizeTitle(dict.getOrDefault(id))).first->second;
      };

      for (auto const& term : sortBy)
      {
        switch (term.field)
        {
          case TrackSortField::Year: keys.year = view.metadata().year(); break;
          case TrackSortField::DiscNumber: keys.discNumber = view.metadata().discNumber(); break;
          case TrackSortField::TrackNumber: keys.trackNumber = view.metadata().trackNumber(); break;
          case TrackSortField::Duration: keys.durationMs = view.property().durationMs(); break;
          case TrackSortField::Title:
            keys.titleKey = intern(stringPool, normalizeTitle(view.metadata().title()));
            break;
          case TrackSortField::Artist: keys.artistKey = getNorm(view.metadata().artistId()); break;
          case TrackSortField::Album: keys.albumKey = getNorm(view.metadata().albumId()); break;
          case TrackSortField::AlbumArtist: keys.albumArtistKey = getNorm(view.metadata().albumArtistId()); break;
          case TrackSortField::Genre: keys.genreKey = getNorm(view.metadata().genreId()); break;
          case TrackSortField::Composer: keys.composerKey = getNorm(view.metadata().composerId()); break;
          case TrackSortField::Work: keys.workKey = getNorm(view.metadata().workId()); break;
        }
      }
    }

    void ensureGroupSortKeys(SortKeys& keys,
                             ao::library::TrackView const& view,
                             ao::library::DictionaryStore& dict,
                             TrackGroupKey groupBy,
                             std::unordered_map<ao::DictionaryId, std::string>& normCache)
    {
      auto const getNorm = [&](ao::DictionaryId id) -> std::string_view
      {
        if (auto it = normCache.find(id); it != normCache.end())
        {
          return it->second;
        }

        return normCache.emplace(id, normalizeTitle(dict.getOrDefault(id))).first->second;
      };

      switch (groupBy)
      {
        case TrackGroupKey::Artist:
          if (keys.artistKey.empty())
          {
            keys.artistKey = getNorm(view.metadata().artistId());
          }

          break;
        case TrackGroupKey::Album:
          if (keys.albumKey.empty())
          {
            keys.albumKey = getNorm(view.metadata().albumId());
          }

          if (keys.albumArtistKey.empty())
          {
            keys.albumArtistKey = getNorm(view.metadata().albumArtistId());
          }

          break;
        case TrackGroupKey::AlbumArtist:
          if (keys.albumArtistKey.empty())
          {
            keys.albumArtistKey = getNorm(view.metadata().albumArtistId());
          }

          break;
        case TrackGroupKey::Genre:
          if (keys.genreKey.empty())
          {
            keys.genreKey = getNorm(view.metadata().genreId());
          }

          break;
        case TrackGroupKey::Composer:
          if (keys.composerKey.empty())
          {
            keys.composerKey = getNorm(view.metadata().composerId());
          }

          break;
        case TrackGroupKey::Work:
          if (keys.workKey.empty())
          {
            keys.workKey = getNorm(view.metadata().workId());
          }

          break;
        default: break;
      }
    }

    void fillGroupMetadata(OrderEntry& entry,
                           ao::library::TrackView const& view,
                           ao::library::DictionaryStore& dict,
                           TrackGroupKey groupBy,
                           std::unordered_set<std::string>& stringPool)
    {
      switch (groupBy)
      {
        case TrackGroupKey::None: return;
        case TrackGroupKey::Artist:
          entry.groupKey = entry.keys.artistKey;
          entry.groupLabel = dict.getOrDefault(view.metadata().artistId(), "Unknown Artist");
          break;
        case TrackGroupKey::Album:
          entry.groupKey =
            intern(stringPool, std::string{entry.keys.albumArtistKey} + "\x1F" + std::string{entry.keys.albumKey});
          {
            std::string album{dict.getOrDefault(view.metadata().albumId())};
            std::string albumArtist{dict.getOrDefault(view.metadata().albumArtistId())};

            if (entry.keys.albumKey.empty())
            {
              entry.groupLabel = "Unknown Album";
            }
            else if (entry.keys.albumArtistKey.empty())
            {
              entry.groupLabel = intern(stringPool, std::move(album));
            }
            else
            {
              entry.groupLabel = intern(stringPool, album + " - " + albumArtist);
            }
          }

          break;
        case TrackGroupKey::AlbumArtist:
          entry.groupKey = entry.keys.albumArtistKey;
          entry.groupLabel = dict.getOrDefault(view.metadata().albumArtistId(), "Unknown Artist");
          break;
        case TrackGroupKey::Genre:
          entry.groupKey = entry.keys.genreKey;
          entry.groupLabel = dict.getOrDefault(view.metadata().genreId(), "Unknown Genre");
          break;
        case TrackGroupKey::Composer:
          entry.groupKey = entry.keys.composerKey;
          entry.groupLabel = dict.getOrDefault(view.metadata().composerId(), "Unknown Composer");
          break;
        case TrackGroupKey::Work:
          entry.groupKey = entry.keys.workKey;
          entry.groupLabel = dict.getOrDefault(view.metadata().workId(), "Unknown Work");
          break;
        case TrackGroupKey::Year:
        {
          std::uint16_t const year = entry.keys.year;
          entry.groupKey = intern(stringPool, std::format("{:05d}", year));
          entry.groupLabel = (year == 0) ? "Unknown Year" : intern(stringPool, std::format("{}", year));
        }

        break;
        default: break;
      }
    }

    bool sameSortDirection(std::vector<TrackSortTerm> const& old, std::vector<TrackSortTerm> const& updated)
    {
      if (old.size() != updated.size())
      {
        return false;
      }

      for (std::size_t idx = 0; idx < old.size(); ++idx)
      {
        if (old[idx].field != updated[idx].field)
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
    TrackSource& source;
    ao::library::MusicLibrary& library;
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy;
    Comparator comparator;
    ao::library::TrackStore::Reader::LoadMode loadMode = ao::library::TrackStore::Reader::LoadMode::Hot;
    std::vector<OrderEntry> orderIndex;
    std::unordered_map<ao::TrackId, std::size_t> positionIndex;
    std::vector<GroupSection> sections;
    std::uint64_t rev = 0;
    std::unordered_map<ao::DictionaryId, std::string> normCache;
    std::unordered_set<std::string> stringPool;
    std::vector<std::move_only_function<void(TrackListProjectionDeltaBatch const&)>> subscribers;

    void rebuildGroups()
    {
      if (groupBy == TrackGroupKey::None || orderIndex.empty())
      {
        sections.clear();
        return;
      }

      auto txn = library.readTransaction();
      auto reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      for (auto& entry : orderIndex)
      {
        auto view = reader.get(entry.trackId, loadMode);

        if (view)
        {
          ensureGroupSortKeys(entry.keys, *view, dict, groupBy, normCache);
          fillGroupMetadata(entry, *view, dict, groupBy, stringPool);
        }
      }
    }

    Impl(ViewId vid, TrackSource& src, ao::library::MusicLibrary& lib)
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

    void buildGroupSections()
    {
      sections.clear();

      if (orderIndex.empty() || groupBy == TrackGroupKey::None)
      {
        return;
      }

      sections.push_back(GroupSection{
        .rows = {0, 1},
        .label = orderIndex[0].groupLabel,
      });

      for (std::size_t idx = 1; idx < orderIndex.size(); ++idx)
      {
        if (orderIndex[idx].groupKey != orderIndex[idx - 1].groupKey)
        {
          sections.push_back(GroupSection{
            .rows = {idx, 1},
            .label = orderIndex[idx].groupLabel,
          });
        }
        else
        {
          sections.back().rows.count++;
        }
      }
    }

    void rebuildOrderIndex()
    {
      auto const timer = ao::utility::ScopedTimer{"TrackListProjection::rebuildOrderIndex"};
      orderIndex.clear();
      positionIndex.clear();
      sections.clear();
      orderIndex.reserve(source.size());

      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      for (std::size_t idx = 0; idx < source.size(); ++idx)
      {
        auto const trackId = source.trackIdAt(idx);
        auto const optView = reader.get(trackId, loadMode);

        if (optView)
        {
          OrderEntry entry{.trackId = trackId};
          fillSortKeys(entry.keys, *optView, dict, sortBy, normCache, stringPool);
          if (groupBy != TrackGroupKey::None)
          {
            ensureGroupSortKeys(entry.keys, *optView, dict, groupBy, normCache);
            fillGroupMetadata(entry, *optView, dict, groupBy, stringPool);
          }
          orderIndex.push_back(entry);
        }
      }

      if (comparator)
      {
        std::ranges::sort(orderIndex, std::ref(comparator));
      }

      rebuildPositionIndex();
      buildGroupSections();
    }

    void rebuildPositionIndex()
    {
      positionIndex.clear();
      positionIndex.reserve(orderIndex.size());
      for (auto const& [idx, entry] : std::views::enumerate(orderIndex))
      {
        positionIndex[entry.trackId] = idx;
      }
    }

    std::optional<std::size_t> findPosition(ao::TrackId trackId) const
    {
      if (auto it = positionIndex.find(trackId); it != positionIndex.end())
      {
        return it->second;
      }

      return std::nullopt;
    }

    bool sectionsEqual(std::vector<GroupSection> const& left, std::vector<GroupSection> const& right) const
    {
      if (left.size() != right.size())
      {
        return false;
      }

      for (std::size_t idx = 0; idx < left.size(); ++idx)
      {
        if (left[idx].rows.start != right[idx].rows.start || left[idx].rows.count != right[idx].rows.count ||
            left[idx].label != right[idx].label)
        {
          return false;
        }
      }

      return true;
    }

    void insertEntry(ao::TrackId trackId)
    {
      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto const optView = reader.get(trackId, loadMode);

      if (!optView)
      {
        return;
      }

      OrderEntry entry{.trackId = trackId};
      fillSortKeys(entry.keys, *optView, dict, sortBy, normCache, stringPool);
      if (groupBy != TrackGroupKey::None)
      {
        ensureGroupSortKeys(entry.keys, *optView, dict, groupBy, normCache);
        fillGroupMetadata(entry, *optView, dict, groupBy, stringPool);
      }

      std::size_t pos = 0;

      if (comparator)
      {
        auto it = std::ranges::lower_bound(orderIndex, entry, std::ref(comparator));
        pos = static_cast<std::size_t>(it - orderIndex.begin());
        orderIndex.insert(it, entry);
      }
      else
      {
        pos = orderIndex.size();
        orderIndex.push_back(entry);
      }

      for (std::size_t idx = pos; idx < orderIndex.size(); ++idx)
      {
        positionIndex[orderIndex[idx].trackId] = idx;
      }

      if (groupBy != TrackGroupKey::None)
      {
        auto oldSections = sections;
        buildGroupSections();
        if (!sectionsEqual(oldSections, sections))
        {
          publishDelta(TrackListProjectionDeltaBatch{
            .revision = ++rev,
            .deltas = {ProjectionReset{}},
          });
          return;
        }
      }

      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionInsertRange{Range{.start = pos, .count = 1}}},
      });
    }

    void insertEntries(std::span<ao::TrackId const> ids)
    {
      if (ids.empty())
      {
        return;
      }

      if (ids.size() == 1)
      {
        insertEntry(ids[0]);
        return;
      }

      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto sortedNew = std::vector<OrderEntry>{};
      sortedNew.reserve(ids.size());

      for (auto const id : ids)
      {
        auto const optView = reader.get(id, loadMode);

        if (optView)
        {
          OrderEntry entry{.trackId = id};
          fillSortKeys(entry.keys, *optView, dict, sortBy, normCache, stringPool);
          if (groupBy != TrackGroupKey::None)
          {
            ensureGroupSortKeys(entry.keys, *optView, dict, groupBy, normCache);
            fillGroupMetadata(entry, *optView, dict, groupBy, stringPool);
          }
          sortedNew.push_back(entry);
        }
      }

      if (sortedNew.empty())
      {
        return;
      }

      if (comparator)
      {
        std::ranges::sort(sortedNew, std::ref(comparator));
      }

      if (groupBy != TrackGroupKey::None)
      {
        rebuildOrderIndex();
        publishDelta(TrackListProjectionDeltaBatch{
          .revision = ++rev,
          .deltas = {ProjectionReset{}},
        });
        return;
      }

      std::vector<OrderEntry> merged;
      merged.reserve(orderIndex.size() + sortedNew.size());

      if (comparator)
      {
        std::ranges::merge(orderIndex, sortedNew, std::back_inserter(merged), std::ref(comparator));
      }
      else
      {
        merged = std::move(orderIndex);
        merged.insert(
          merged.end(), std::make_move_iterator(sortedNew.begin()), std::make_move_iterator(sortedNew.end()));
      }

      orderIndex = std::move(merged);
      rebuildPositionIndex();

      // For now, always use ProjectionReset for multiple insertions to simplify UI sync.
      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionReset{}},
      });
    }

    void removeEntry(ao::TrackId trackId)
    {
      auto const optPos = findPosition(trackId);
      if (!optPos)
      {
        return;
      }

      std::size_t const pos = *optPos;
      orderIndex.erase(orderIndex.begin() + static_cast<std::ptrdiff_t>(pos));
      positionIndex.erase(trackId);

      for (std::size_t idx = pos; idx < orderIndex.size(); ++idx)
      {
        positionIndex[orderIndex[idx].trackId] = idx;
      }

      if (groupBy != TrackGroupKey::None)
      {
        auto oldSections = sections;
        buildGroupSections();
        if (!sectionsEqual(oldSections, sections))
        {
          publishDelta(TrackListProjectionDeltaBatch{
            .revision = ++rev,
            .deltas = {ProjectionReset{}},
          });
          return;
        }
      }

      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionRemoveRange{Range{.start = pos, .count = 1}}},
      });
    }

    void removeEntries(std::span<ao::TrackId const> ids)
    {
      if (ids.empty())
      {
        return;
      }

      if (ids.size() == 1)
      {
        removeEntry(ids[0]);
        return;
      }

      auto positions = std::vector<std::size_t>{};
      positions.reserve(ids.size());

      for (auto id : ids)
      {
        if (auto optPos = findPosition(id))
        {
          positions.push_back(*optPos);
        }
      }

      if (positions.empty())
      {
        return;
      }

      std::ranges::sort(positions, std::greater<>{});

      for (auto pos : positions)
      {
        auto id = orderIndex[pos].trackId;
        orderIndex.erase(orderIndex.begin() + static_cast<std::ptrdiff_t>(pos));
        positionIndex.erase(id);
      }

      rebuildPositionIndex();

      if (groupBy != TrackGroupKey::None)
      {
        buildGroupSections();
      }

      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionReset{}},
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

      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto const optView = reader.get(trackId, loadMode);

      if (!optView)
      {
        return;
      }

      auto newKeys = SortKeys{};
      fillSortKeys(newKeys, *optView, dict, sortBy, normCache, stringPool);

      if (groupBy != TrackGroupKey::None)
      {
        rebuildOrderIndex();
        publishDelta(TrackListProjectionDeltaBatch{
          .revision = ++rev,
          .deltas = {ProjectionReset{}},
        });
        return;
      }

      if (comparator)
      {
        auto testEntry = OrderEntry{.trackId = trackId, .keys = newKeys};

        if (comparator(oldEntry, testEntry) == comparator(testEntry, oldEntry))
        {
          oldEntry.keys = testEntry.keys;
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

      oldEntry.keys = newKeys;
      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionUpdateRange{Range{.start = oldPos, .count = 1}}},
      });
    }

    void updateEntries(std::span<ao::TrackId const> ids)
    {
      if (ids.empty())
      {
        return;
      }

      if (ids.size() == 1)
      {
        updateEntry(ids[0]);
        return;
      }

      // If many entries are updated, it's easier to just rebuild.
      // But we can still avoid full re-fetching from DB for UNCHANGED tracks.
      // However, we don't know which ones are unchanged without checking.

      // For now, let's just rebuild if multiple updates occur.
      rebuildOrderIndex();
      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionReset{}},
      });
    }
  };

  TrackListProjection::TrackListProjection(ViewId viewId, TrackSource& source, ao::library::MusicLibrary& library)
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

  void TrackListProjection::setPresentation(TrackGroupKey groupBy, std::vector<TrackSortTerm> sortBy)
  {
    if (_impl->groupBy == groupBy && sameSortDirection(_impl->sortBy, sortBy) && _impl->comparator)
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

    _impl->groupBy = groupBy;
    _impl->sortBy = std::move(sortBy);
    _impl->comparator = buildComparator(_impl->sortBy);
    _impl->loadMode = computeLoadMode(_impl->sortBy, _impl->groupBy);
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

  std::optional<std::size_t> TrackListProjection::indexOf(ao::TrackId trackId) const noexcept
  {
    if (auto const it = _impl->positionIndex.find(trackId); it != _impl->positionIndex.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  TrackListPresentationSnapshot TrackListProjection::presentation() const
  {
    return TrackListPresentationSnapshot{
      .groupBy = _impl->groupBy,
      .effectiveSortBy = _impl->sortBy,
      .redundantFields = presentationForGroup(_impl->groupBy).redundantFields,
      .revision = _impl->rev,
    };
  }

  std::size_t TrackListProjection::groupCount() const noexcept
  {
    return _impl->sections.size();
  }

  TrackGroupSectionSnapshot TrackListProjection::groupAt(std::size_t groupIndex) const
  {
    if (groupIndex >= _impl->sections.size())
    {
      return {};
    }

    auto const& section = _impl->sections[groupIndex];
    return TrackGroupSectionSnapshot{
      .rows = section.rows,
      .label = std::string{section.label},
    };
  }

  std::optional<std::size_t> TrackListProjection::groupIndexAt(std::size_t rowIndex) const
  {
    for (std::size_t idx = 0; idx < _impl->sections.size(); ++idx)
    {
      auto const& section = _impl->sections[idx];

      if (rowIndex >= section.rows.start && rowIndex < section.rows.start + section.rows.count)
      {
        return idx;
      }
    }

    return std::nullopt;
  }

  Subscription TrackListProjection::subscribe(
    std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler)
  {
    handler(TrackListProjectionDeltaBatch{
      .revision = _impl->rev,
      .deltas = {ProjectionReset{}},
    });

    _impl->subscribers.push_back(std::move(handler));
    std::size_t const idx = _impl->subscribers.size() - 1;

    return Subscription{[this, idx] { _impl->subscribers[idx] = {}; }};
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

  void TrackListProjection::onInserted(std::span<ao::TrackId const> ids)
  {
    _impl->insertEntries(ids);
  }

  void TrackListProjection::onUpdated(std::span<ao::TrackId const> ids)
  {
    _impl->updateEntries(ids);
  }

  void TrackListProjection::onRemoved(std::span<ao::TrackId const> ids)
  {
    _impl->removeEntries(ids);
  }

  void TrackListProjection::publishDelta(TrackListProjectionDeltaBatch const& batch)
  {
    _impl->publishDelta(batch);
  }
}
