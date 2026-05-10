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

    struct GroupSection final
    {
      Range rows{};
      std::string label{};
    };

    struct OrderEntry final
    {
      ao::TrackId trackId{};
      SortKeys keys{};
      std::string groupKey{};
      std::string groupLabel{};
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

    auto groupByNeedsCold(TrackGroupKey groupBy) -> bool
    {
      return groupBy == TrackGroupKey::Work;
    }

    auto computeLoadMode(std::vector<TrackSortTerm> const& sortBy, TrackGroupKey groupBy)
      -> ao::library::TrackStore::Reader::LoadMode
    {
      auto needsHot = false;
      auto needsCold = groupByNeedsCold(groupBy);

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
        return lhs.trackId < rhs.trackId;
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
          case TrackSortField::Artist:
            keys.artistKey = normalizeTitle(dict.getOrDefault(view.metadata().artistId()));
            break;
          case TrackSortField::Album:
            keys.albumKey = normalizeTitle(dict.getOrDefault(view.metadata().albumId()));
            break;
          case TrackSortField::AlbumArtist:
            keys.albumArtistKey = normalizeTitle(dict.getOrDefault(view.metadata().albumArtistId()));
            break;
          case TrackSortField::Genre:
            keys.genreKey = normalizeTitle(dict.getOrDefault(view.metadata().genreId()));
            break;
          case TrackSortField::Composer:
            keys.composerKey = normalizeTitle(dict.getOrDefault(view.metadata().composerId()));
            break;
          case TrackSortField::Work: keys.workKey = normalizeTitle(dict.getOrDefault(view.metadata().workId())); break;
        }
      }
    }

    void ensureGroupSortKeys(SortKeys& keys,
                             ao::library::TrackView const& view,
                             ao::library::DictionaryStore& dict,
                             TrackGroupKey groupBy)
    {
      switch (groupBy)
      {
        case TrackGroupKey::Artist:
          if (keys.artistKey.empty())
          {
            keys.artistKey = normalizeTitle(dict.getOrDefault(view.metadata().artistId()));
          }
          break;
        case TrackGroupKey::Album:
          if (keys.albumKey.empty())
          {
            keys.albumKey = normalizeTitle(dict.getOrDefault(view.metadata().albumId()));
          }
          if (keys.albumArtistKey.empty())
          {
            keys.albumArtistKey = normalizeTitle(dict.getOrDefault(view.metadata().albumArtistId()));
          }
          break;
        case TrackGroupKey::AlbumArtist:
          if (keys.albumArtistKey.empty())
          {
            keys.albumArtistKey = normalizeTitle(dict.getOrDefault(view.metadata().albumArtistId()));
          }
          break;
        case TrackGroupKey::Genre:
          if (keys.genreKey.empty())
          {
            keys.genreKey = normalizeTitle(dict.getOrDefault(view.metadata().genreId()));
          }
          break;
        case TrackGroupKey::Composer:
          if (keys.composerKey.empty())
          {
            keys.composerKey = normalizeTitle(dict.getOrDefault(view.metadata().composerId()));
          }
          break;
        case TrackGroupKey::Work:
          if (keys.workKey.empty())
          {
            keys.workKey = normalizeTitle(dict.getOrDefault(view.metadata().workId()));
          }
          break;
        default: break;
      }
    }

    void fillGroupData(OrderEntry& entry,
                       ao::library::TrackView const& view,
                       ao::library::DictionaryStore& dict,
                       TrackGroupKey groupBy)
    {
      switch (groupBy)
      {
        case TrackGroupKey::None: return;
        case TrackGroupKey::Artist:
          entry.groupKey = entry.keys.artistKey;
          entry.groupLabel =
            entry.keys.artistKey.empty() ? "Unknown Artist" : dict.getOrDefault(view.metadata().artistId());
          break;
        case TrackGroupKey::Album:
          entry.groupKey = entry.keys.albumArtistKey + "\x1F" + entry.keys.albumKey;
          {
            auto album = std::string{dict.getOrDefault(view.metadata().albumId())};
            auto albumArtist = std::string{dict.getOrDefault(view.metadata().albumArtistId())};
            if (entry.keys.albumKey.empty())
            {
              entry.groupLabel = "Unknown Album";
            }
            else if (entry.keys.albumArtistKey.empty())
            {
              entry.groupLabel = std::move(album);
            }
            else
            {
              entry.groupLabel = std::move(album) + " - " + std::move(albumArtist);
            }
          }
          break;
        case TrackGroupKey::AlbumArtist:
          entry.groupKey = entry.keys.albumArtistKey;
          entry.groupLabel = entry.keys.albumArtistKey.empty() ? "Unknown Album Artist"
                                                               : dict.getOrDefault(view.metadata().albumArtistId());
          break;
        case TrackGroupKey::Genre:
          entry.groupKey = entry.keys.genreKey;
          entry.groupLabel =
            entry.keys.genreKey.empty() ? "Unknown Genre" : dict.getOrDefault(view.metadata().genreId());
          break;
        case TrackGroupKey::Composer:
          entry.groupKey = entry.keys.composerKey;
          entry.groupLabel =
            entry.keys.composerKey.empty() ? "Unknown Composer" : dict.getOrDefault(view.metadata().composerId());
          break;
        case TrackGroupKey::Work:
          entry.groupKey = entry.keys.workKey;
          entry.groupLabel = entry.keys.workKey.empty() ? "Unknown Work" : dict.getOrDefault(view.metadata().workId());
          break;
        case TrackGroupKey::Year:
        {
          auto const year = entry.keys.year;
          entry.groupKey = std::format("{:05d}", year);
          entry.groupLabel = (year == 0) ? "Unknown Year" : std::format("{}", year);
        }
        break;
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
    std::vector<std::move_only_function<void(TrackListProjectionDeltaBatch const&)>> subscribers;

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

      for (auto i = std::size_t{1}; i < orderIndex.size(); ++i)
      {
        if (orderIndex[i].groupKey != orderIndex[i - 1].groupKey)
        {
          sections.push_back(GroupSection{
            .rows = {i, 1},
            .label = orderIndex[i].groupLabel,
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
      orderIndex.clear();
      positionIndex.clear();
      sections.clear();
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
        ensureGroupSortKeys(entry.keys, *optView, dict, groupBy);
        fillGroupData(entry, *optView, dict, groupBy);

        orderIndex.push_back(std::move(entry));
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

    auto sectionsEqual(std::vector<GroupSection> const& left, std::vector<GroupSection> const& right) const -> bool
    {
      if (left.size() != right.size())
      {
        return false;
      }
      for (auto i = std::size_t{0}; i < left.size(); ++i)
      {
        if (left[i].rows.start != right[i].rows.start || left[i].rows.count != right[i].rows.count ||
            left[i].label != right[i].label)
        {
          return false;
        }
      }
      return true;
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
      ensureGroupSortKeys(entry.keys, *optView, dict, groupBy);
      fillGroupData(entry, *optView, dict, groupBy);

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
    auto const it = _impl->positionIndex.find(trackId);
    if (it == _impl->positionIndex.end())
    {
      return std::nullopt;
    }
    return it->second;
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
      .label = section.label,
    };
  }

  std::optional<std::size_t> TrackListProjection::groupIndexAt(std::size_t rowIndex) const
  {
    for (auto i = std::size_t{0}; i < _impl->sections.size(); ++i)
    {
      auto const& section = _impl->sections[i];
      if (rowIndex >= section.rows.start && rowIndex < section.rows.start + section.rows.count)
      {
        return i;
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
