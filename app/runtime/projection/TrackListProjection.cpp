// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/utility/StringArena.h>

#include <boost/container/small_vector.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    constexpr std::size_t kYearStrLen = 5;

    struct SortKeys final
    {
      std::uint16_t year = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t trackNumber = 0;
      std::uint16_t movementNumber = 0;
      std::chrono::milliseconds duration{0};
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
      std::string_view groupKey{};
      std::string_view primaryText{};
      std::string_view secondaryText{};
      std::string_view tertiaryText{};
      ResourceId imageId{kInvalidResourceId};
    };

    struct OrderEntry final
    {
      TrackId trackId{};
      SortKeys keys{};
      std::string_view groupKey{};
      std::string_view primaryText{};
      std::string_view secondaryText{};
      std::string_view tertiaryText{};
      ResourceId imageId{kInvalidResourceId};
    };

    using Comparator = std::move_only_function<bool(OrderEntry const&, OrderEntry const&)>;

    constexpr std::size_t kArticleAnLength = 3;

    void sortUniquePositions(std::vector<std::size_t>& positions)
    {
      std::ranges::sort(positions);
      positions.erase(std::ranges::unique(positions).begin(), positions.end());
    }

    void appendAscendingRanges(boost::container::small_vector<TrackListProjectionDelta, 1>& deltas,
                               std::vector<std::size_t>& positions,
                               auto makeDelta)
    {
      if (positions.empty())
      {
        return;
      }

      sortUniquePositions(positions);

      auto start = positions.front();
      auto prev = start;

      for (auto const pos : positions | std::views::drop(1))
      {
        if (pos == prev + 1)
        {
          prev = pos;
          continue;
        }

        deltas.push_back(makeDelta(Range{.start = start, .count = prev - start + 1}));
        start = pos;
        prev = pos;
      }

      deltas.push_back(makeDelta(Range{.start = start, .count = prev - start + 1}));
    }

    void appendRemovalRangesDescending(boost::container::small_vector<TrackListProjectionDelta, 1>& deltas,
                                       std::vector<std::size_t>& positions)
    {
      if (positions.empty())
      {
        return;
      }

      sortUniquePositions(positions);
      std::ranges::reverse(positions);

      auto high = positions.front();
      auto low = high;

      for (auto const pos : positions | std::views::drop(1))
      {
        if (pos + 1 == low)
        {
          low = pos;
          continue;
        }

        deltas.push_back(ProjectionRemoveRange{Range{.start = low, .count = high - low + 1}});
        high = pos;
        low = pos;
      }

      deltas.push_back(ProjectionRemoveRange{Range{.start = low, .count = high - low + 1}});
    }

    bool iStartsWith(std::string_view str, std::string_view prefix)
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

    // Normalize a title into a caller-owned scratch buffer: strip a leading article and
    // lower-case the rest. Writing into a reused buffer avoids a per-call string allocation;
    // the result is meant to be interned immediately, not retained.
    void normalizeInto(std::string& out, std::string_view title)
    {
      std::size_t offset = 0;

      if (iStartsWith(title, "the "))
      {
        offset = 4;
      }
      else if (iStartsWith(title, "a "))
      {
        offset = 2;
      }
      else if (iStartsWith(title, "an "))
      {
        offset = kArticleAnLength;
      }

      auto const body = title.substr(offset);
      out.clear();
      out.reserve(body.size());

      for (auto const ch : body)
      {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
    }

    bool sortFieldNeedsCold(TrackSortField field)
    {
      switch (field)
      {
        case TrackSortField::Duration:
        case TrackSortField::DiscNumber:
        case TrackSortField::TrackNumber:
        case TrackSortField::Work:
        case TrackSortField::Movement: return true;
        default: return false;
      }
    }

    bool groupByNeedsCold(TrackGroupKey groupBy)
    {
      return groupBy == TrackGroupKey::Work || groupBy == TrackGroupKey::Album;
    }

    library::TrackStore::Reader::LoadMode computeLoadMode(std::vector<TrackSortTerm> const& sortBy,
                                                          TrackGroupKey groupBy)
    {
      bool needsHot = groupBy != TrackGroupKey::None;
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
        return library::TrackStore::Reader::LoadMode::Hot;
      }

      if (needsHot && needsCold)
      {
        return library::TrackStore::Reader::LoadMode::Both;
      }

      if (needsCold)
      {
        return library::TrackStore::Reader::LoadMode::Cold;
      }

      return library::TrackStore::Reader::LoadMode::Hot;
    }

    std::int32_t compareNumeric(auto lhsVal, auto rhsVal)
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

    std::int32_t compareSingleField(TrackSortTerm const& term, SortKeys const& lhs, SortKeys const& rhs)
    {
      switch (term.field)
      {
        case TrackSortField::Year: return compareNumeric(lhs.year, rhs.year);
        case TrackSortField::DiscNumber: return compareNumeric(lhs.discNumber, rhs.discNumber);
        case TrackSortField::TrackNumber: return compareNumeric(lhs.trackNumber, rhs.trackNumber);
        case TrackSortField::Movement: return compareNumeric(lhs.movementNumber, rhs.movementNumber);
        case TrackSortField::Duration: return compareNumeric(lhs.duration, rhs.duration);
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
          if (auto const cmp = compareSingleField(term, lhs.keys, rhs.keys); cmp != 0)
          {
            return term.ascending ? (cmp < 0) : (cmp > 0);
          }
        }

        return lhs.trackId < rhs.trackId;
      };
    }

    // Resolve a dictionary id to its normalized sort key, cached per id. The normalized text
    // is interned into the shared arena so the cache stores a stable view rather than owning
    // a string; repeated ids and repeated content both reuse the same arena bytes.
    using NormCache = boost::unordered_flat_map<DictionaryId, std::string_view, std::hash<DictionaryId>>;

    std::string_view normalizeCached(NormCache& normCache,
                                     utility::StringArena& arena,
                                     std::string& scratch,
                                     library::DictionaryStore& dict,
                                     DictionaryId id)
    {
      if (auto const it = normCache.find(id); it != normCache.end())
      {
        return it->second;
      }

      normalizeInto(scratch, dict.getOrDefault(id));
      auto const view = arena.intern(scratch);
      normCache.emplace(id, view);
      return view;
    }

    void fillSortKeys(SortKeys& keys,
                      library::TrackView const& view,
                      library::DictionaryStore& dict,
                      std::vector<TrackSortTerm> const& sortBy,
                      NormCache& normCache,
                      utility::StringArena& arena,
                      std::string& scratch)
    {
      auto const getNorm = [&](DictionaryId id) -> std::string_view
      { return normalizeCached(normCache, arena, scratch, dict, id); };

      for (auto const& term : sortBy)
      {
        switch (term.field)
        {
          case TrackSortField::Year: keys.year = view.metadata().year(); break;
          case TrackSortField::DiscNumber: keys.discNumber = view.metadata().discNumber(); break;
          case TrackSortField::TrackNumber: keys.trackNumber = view.metadata().trackNumber(); break;
          case TrackSortField::Movement: keys.movementNumber = view.metadata().movementNumber(); break;
          case TrackSortField::Duration: keys.duration = view.property().duration(); break;
          case TrackSortField::Title:
            normalizeInto(scratch, view.metadata().title());
            keys.titleKey = arena.intern(scratch);
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
                             library::TrackView const& view,
                             library::DictionaryStore& dict,
                             TrackGroupKey groupBy,
                             NormCache& normCache,
                             utility::StringArena& arena,
                             std::string& scratch)
    {
      auto const getNorm = [&](DictionaryId id) -> std::string_view
      { return normalizeCached(normCache, arena, scratch, dict, id); };

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

          if (keys.composerKey.empty())
          {
            keys.composerKey = getNorm(view.metadata().composerId());
          }

          break;
        default: break;
      }
    }

    std::string_view internCompoundKey(utility::StringArena& arena,
                                       std::string& scratch,
                                       std::string_view lhs,
                                       std::string_view rhs)
    {
      scratch.clear();
      scratch.reserve(lhs.size() + 1 + rhs.size());
      scratch.append(lhs);
      scratch.push_back('\x1F');
      scratch.append(rhs);
      return arena.intern(scratch);
    }

    std::string_view internPaddedYearKey(utility::StringArena& arena, std::uint16_t year)
    {
      auto buf = std::array<char, kYearStrLen>{};
      auto const res = std::format_to_n(buf.data(), buf.size(), "{:05}", year);
      return arena.intern(std::string_view{buf.data(), static_cast<std::size_t>(res.out - buf.data())});
    }

    std::string_view internYearText(utility::StringArena& arena, std::uint16_t year)
    {
      auto buf = std::array<char, kYearStrLen>{};
      auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), year);
      std::ignore = ec;
      return arena.intern(std::string_view{buf.data(), static_cast<std::size_t>(ptr - buf.data())});
    }

    void fillGroupMetadata(OrderEntry& entry,
                           library::TrackView const& view,
                           library::DictionaryStore& dict,
                           TrackGroupKey groupBy,
                           utility::StringArena& arena,
                           std::string& scratch)
    {
      switch (groupBy)
      {
        case TrackGroupKey::None: return;
        case TrackGroupKey::Artist:
          entry.groupKey = entry.keys.artistKey;
          entry.primaryText = dict.getOrDefault(view.metadata().artistId(), "Unknown Artist");
          break;
        case TrackGroupKey::Album:
          entry.groupKey = internCompoundKey(arena, scratch, entry.keys.albumArtistKey, entry.keys.albumKey);

          if (auto const optPrimary = view.coverArt().primary(); optPrimary)
          {
            entry.imageId = optPrimary->resourceId;
          }

          {
            auto album = std::string{dict.getOrDefault(view.metadata().albumId())};
            auto albumArtist = std::string{dict.getOrDefault(view.metadata().albumArtistId())};

            if (entry.keys.albumKey.empty())
            {
              entry.primaryText = "Unknown Album";
            }
            else
            {
              entry.primaryText = arena.intern(std::move(album));
            }

            if (entry.keys.albumArtistKey.empty())
            {
              entry.secondaryText = "Unknown Artist";
            }
            else
            {
              entry.secondaryText = arena.intern(std::move(albumArtist));
            }

            if (auto year = view.metadata().year(); year != 0)
            {
              entry.tertiaryText = internYearText(arena, year);
            }
            else
            {
              entry.tertiaryText = "Unknown Year";
            }
          }

          break;
        case TrackGroupKey::AlbumArtist:
          entry.groupKey = entry.keys.albumArtistKey;
          entry.primaryText = dict.getOrDefault(view.metadata().albumArtistId(), "Unknown Artist");
          break;
        case TrackGroupKey::Genre:
          entry.groupKey = entry.keys.genreKey;
          entry.primaryText = dict.getOrDefault(view.metadata().genreId(), "Unknown Genre");
          break;
        case TrackGroupKey::Composer:
          entry.groupKey = entry.keys.composerKey;
          entry.primaryText = dict.getOrDefault(view.metadata().composerId(), "Unknown Composer");
          break;
        case TrackGroupKey::Work:
          entry.groupKey = internCompoundKey(arena, scratch, entry.keys.composerKey, entry.keys.workKey);
          entry.primaryText = dict.getOrDefault(view.metadata().workId(), "Unknown Work");
          entry.secondaryText = dict.getOrDefault(view.metadata().composerId(), "Unknown Composer");
          break;
        case TrackGroupKey::Year:
        {
          std::uint16_t const year = entry.keys.year;
          entry.groupKey = internPaddedYearKey(arena, year);
          entry.primaryText = (year == 0) ? "Unknown Year" : internYearText(arena, year);
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
  } // namespace

  struct TrackListProjection::Impl final
  {
    ViewId viewId;
    TrackSource& source;
    library::MusicLibrary& library;
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy;
    std::string id = std::string{kDefaultTrackPresentationId};
    std::vector<TrackField> visibleFields;
    std::vector<TrackField> redundantFields;
    Comparator comparator;
    library::TrackStore::Reader::LoadMode loadMode = library::TrackStore::Reader::LoadMode::Hot;
    // Sort/group key strings live in the arena (bump-allocated, content-deduplicated); the
    // string_views in orderIndex/sections/normCache below all point into it, so it is declared
    // first to outlive them. normScratch is a reused normalization buffer that keeps the
    // per-track title path allocation-free.
    utility::StringArena stringArena;
    std::string normScratch;
    NormCache normCache;

    std::vector<OrderEntry> orderIndex;
    // Flat (open-addressing) map: contiguous bucket array, so rebuilding the index costs
    // no per-entry node allocation. Values are plain indices that nobody aliases, so the
    // rehash-on-grow relocation is safe here (unlike the arena-backed views, which stay
    // valid only because the arena bytes never move).
    boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>> positionIndex;
    std::vector<GroupSection> sections;
    std::uint64_t rev = 0;
    std::vector<std::move_only_function<void(TrackListProjectionDeltaBatch const&)>> subscribers;
    bool sourceDestroyed = false;

    struct PendingMovedEntry final
    {
      std::size_t oldPos = 0;
      OrderEntry entry;
    };

    OrderEntry buildOrderEntry(TrackId id, library::TrackView const& view, library::DictionaryStore& dict)
    {
      auto entry = OrderEntry{.trackId = id};
      fillSortKeys(entry.keys, view, dict, sortBy, normCache, stringArena, normScratch);

      if (groupBy != TrackGroupKey::None)
      {
        ensureGroupSortKeys(entry.keys, view, dict, groupBy, normCache, stringArena, normScratch);
        fillGroupMetadata(entry, view, dict, groupBy, stringArena, normScratch);
      }

      return entry;
    }

    void mergeEntries(std::vector<OrderEntry>& entries)
    {
      if (entries.empty())
      {
        return;
      }

      if (comparator)
      {
        std::ranges::sort(entries, std::ref(comparator));

        auto merged = std::vector<OrderEntry>{};
        merged.reserve(orderIndex.size() + entries.size());
        std::ranges::merge(orderIndex, entries, std::back_inserter(merged), std::ref(comparator));
        orderIndex = std::move(merged);
      }
      else
      {
        orderIndex.insert(
          orderIndex.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
      }

      rebuildPositionIndex();
    }

    std::vector<std::size_t> mergeEntriesAndCollectPositions(std::vector<OrderEntry>& entries)
    {
      auto ids = std::vector<TrackId>{};
      ids.reserve(entries.size());

      for (auto const& entry : entries)
      {
        ids.push_back(entry.trackId);
      }

      mergeEntries(entries);

      if (groupBy != TrackGroupKey::None)
      {
        buildGroupSections();
      }

      auto positions = std::vector<std::size_t>{};
      positions.reserve(ids.size());

      for (auto const id : ids)
      {
        if (auto const optPos = findPosition(id); optPos)
        {
          positions.push_back(*optPos);
        }
      }

      return positions;
    }

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
        if (auto const optView =
              storageValueOrNullopt(reader.get(entry.trackId, loadMode), "Failed to rebuild track groups");
            optView)
        {
          ensureGroupSortKeys(entry.keys, *optView, dict, groupBy, normCache, stringArena, normScratch);
          fillGroupMetadata(entry, *optView, dict, groupBy, stringArena, normScratch);
        }
      }
    }

    Impl(ViewId vid, TrackSource& src, library::MusicLibrary& lib)
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
        .rows = {.start = 0, .count = 1},
        .groupKey = orderIndex[0].groupKey,
        .primaryText = orderIndex[0].primaryText,
        .secondaryText = orderIndex[0].secondaryText,
        .tertiaryText = orderIndex[0].tertiaryText,
        .imageId = orderIndex[0].imageId,
      });

      for (std::size_t idx = 1; idx < orderIndex.size(); ++idx)
      {
        if (orderIndex[idx].groupKey != orderIndex[idx - 1].groupKey)
        {
          sections.push_back(GroupSection{
            .rows = {.start = idx, .count = 1},
            .groupKey = orderIndex[idx].groupKey,
            .primaryText = orderIndex[idx].primaryText,
            .secondaryText = orderIndex[idx].secondaryText,
            .tertiaryText = orderIndex[idx].tertiaryText,
            .imageId = orderIndex[idx].imageId,
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
      auto const timer = rt::ScopedTimer{"TrackListProjection::rebuildOrderIndex"};
      orderIndex.clear();
      positionIndex.clear();
      sections.clear();

      // A full rebuild discards every container that holds an arena-backed view, so this is
      // the one safe point to reclaim the arena: clear the view holders first, then normCache
      // (whose values are arena views too), then the arena itself. Without this the arena
      // would only grow across presentation switches / resets, trading the allocation wins
      // for unbounded memory. Incremental insert/update/remove must NOT clear: they keep
      // existing entries whose views still point into the arena.
      normCache.clear();
      stringArena.clear();

      orderIndex.reserve(source.size());

      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      for (std::size_t idx = 0; idx < source.size(); ++idx)
      {
        auto const trackId = source.trackIdAt(idx);

        if (auto const optView = storageValueOrNullopt(reader.get(trackId, loadMode), "Failed to rebuild track order");
            optView)
        {
          orderIndex.push_back(buildOrderEntry(trackId, *optView, dict));
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

      for (auto const& [idx, entry] : std::ranges::views::enumerate(orderIndex))
      {
        positionIndex[entry.trackId] = static_cast<std::size_t>(idx);
      }
    }

    std::optional<std::size_t> findPosition(TrackId trackId) const
    {
      if (auto it = positionIndex.find(trackId); it != positionIndex.end())
      {
        return it->second;
      }

      return std::nullopt;
    }

    bool sectionDescriptorsEqual(std::vector<GroupSection> const& left, std::vector<GroupSection> const& right) const
    {
      if (left.size() != right.size())
      {
        return false;
      }

      for (std::size_t idx = 0; idx < left.size(); ++idx)
      {
        if (left[idx].groupKey != right[idx].groupKey || left[idx].primaryText != right[idx].primaryText ||
            left[idx].secondaryText != right[idx].secondaryText || left[idx].tertiaryText != right[idx].tertiaryText ||
            left[idx].imageId != right[idx].imageId)
        {
          return false;
        }
      }

      return true;
    }

    static void assignSectionMetadata(GroupSection& section, OrderEntry const& entry)
    {
      section.groupKey = entry.groupKey;
      section.primaryText = entry.primaryText;
      section.secondaryText = entry.secondaryText;
      section.tertiaryText = entry.tertiaryText;
      section.imageId = entry.imageId;
    }

    static GroupSection makeSection(std::size_t start, OrderEntry const& entry)
    {
      return GroupSection{
        .rows = {.start = start, .count = 1},
        .groupKey = entry.groupKey,
        .primaryText = entry.primaryText,
        .secondaryText = entry.secondaryText,
        .tertiaryText = entry.tertiaryText,
        .imageId = entry.imageId,
      };
    }

    std::optional<std::size_t> findSectionIndexAt(std::size_t row) const
    {
      auto it =
        std::ranges::upper_bound(sections, row, {}, [](GroupSection const& section) { return section.rows.start; });

      if (it == sections.begin())
      {
        return std::nullopt;
      }

      --it;
      auto const idx = static_cast<std::size_t>(it - sections.begin());

      if (auto const& section = sections[idx];
          row >= section.rows.start && row < section.rows.start + section.rows.count)
      {
        return idx;
      }

      return std::nullopt;
    }

    std::size_t sectionInsertionIndex(std::size_t row) const
    {
      auto it =
        std::ranges::upper_bound(sections, row, {}, [](GroupSection const& section) { return section.rows.start; });
      return static_cast<std::size_t>(it - sections.begin());
    }

    void insertGroupSectionRow(std::size_t pos)
    {
      if (groupBy == TrackGroupKey::None)
      {
        return;
      }

      auto const& entry = orderIndex[pos];
      auto const leftSame = pos > 0 && orderIndex[pos - 1].groupKey == entry.groupKey;
      auto const rightSame = pos + 1 < orderIndex.size() && orderIndex[pos + 1].groupKey == entry.groupKey;
      auto const optLeftSection = leftSame ? findSectionIndexAt(pos - 1) : std::optional<std::size_t>{};
      auto const optRightSection = rightSame ? findSectionIndexAt(pos) : std::optional<std::size_t>{};

      for (auto& section : sections)
      {
        if (section.rows.start >= pos)
        {
          ++section.rows.start;
        }
      }

      if (optLeftSection)
      {
        ++sections[*optLeftSection].rows.count;
        return;
      }

      if (optRightSection)
      {
        auto& section = sections[*optRightSection];
        section.rows.start = pos;
        ++section.rows.count;
        assignSectionMetadata(section, entry);
        return;
      }

      auto const insertAt = sectionInsertionIndex(pos);
      sections.insert(sections.begin() + static_cast<std::ptrdiff_t>(insertAt), makeSection(pos, entry));
    }

    void removeGroupSectionRow(std::size_t pos)
    {
      if (groupBy == TrackGroupKey::None)
      {
        return;
      }

      auto const optSection = findSectionIndexAt(pos);

      if (!optSection)
      {
        return;
      }

      auto const sectionIdx = *optSection;
      auto& section = sections[sectionIdx];
      auto shiftFrom = sectionIdx + 1;

      if (section.rows.count == 1)
      {
        sections.erase(sections.begin() + static_cast<std::ptrdiff_t>(sectionIdx));
        shiftFrom = sectionIdx;
      }
      else
      {
        if (section.rows.start == pos)
        {
          assignSectionMetadata(section, orderIndex[pos + 1]);
        }

        --section.rows.count;
      }

      for (std::size_t idx = shiftFrom; idx < sections.size(); ++idx)
      {
        --sections[idx].rows.start;
      }
    }

    void updateGroupSectionRow(std::size_t pos, OrderEntry entry)
    {
      if (groupBy == TrackGroupKey::None)
      {
        orderIndex[pos] = std::move(entry);
        return;
      }

      if (auto const oldGroupKey = orderIndex[pos].groupKey; oldGroupKey == entry.groupKey)
      {
        orderIndex[pos] = std::move(entry);

        if (auto const optSection = findSectionIndexAt(pos); optSection && sections[*optSection].rows.start == pos)
        {
          assignSectionMetadata(sections[*optSection], orderIndex[pos]);
        }

        return;
      }

      removeGroupSectionRow(pos);
      orderIndex[pos] = std::move(entry);
      insertGroupSectionRow(pos);
    }

    std::size_t insertBuiltEntry(OrderEntry entry)
    {
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

      insertGroupSectionRow(pos);

      for (std::size_t idx = pos; idx < orderIndex.size(); ++idx)
      {
        positionIndex[orderIndex[idx].trackId] = idx;
      }

      return pos;
    }

    void eraseEntryAt(std::size_t pos)
    {
      auto const id = orderIndex[pos].trackId;
      removeGroupSectionRow(pos);
      orderIndex.erase(orderIndex.begin() + static_cast<std::ptrdiff_t>(pos));
      positionIndex.erase(id);

      for (std::size_t idx = pos; idx < orderIndex.size(); ++idx)
      {
        positionIndex[orderIndex[idx].trackId] = idx;
      }
    }

    void eraseEntriesAtPositions(std::vector<std::size_t>& positions)
    {
      if (positions.empty())
      {
        return;
      }

      sortUniquePositions(positions);

      auto retained = std::vector<OrderEntry>{};
      retained.reserve(orderIndex.size() - positions.size());

      std::size_t removeIdx = 0;

      for (std::size_t idx = 0; idx < orderIndex.size(); ++idx)
      {
        if (removeIdx < positions.size() && positions[removeIdx] == idx)
        {
          ++removeIdx;
          continue;
        }

        retained.push_back(std::move(orderIndex[idx]));
      }

      orderIndex = std::move(retained);
      rebuildPositionIndex();

      if (groupBy != TrackGroupKey::None)
      {
        buildGroupSections();
      }
    }

    void insertEntry(TrackId trackId)
    {
      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto const optView = storageValueOrNullopt(reader.get(trackId, loadMode), "Failed to insert projected track");

      if (!optView)
      {
        return;
      }

      auto entry = buildOrderEntry(trackId, *optView, dict);
      auto const oldSections = sections;
      auto const pos = insertBuiltEntry(std::move(entry));

      if (groupBy != TrackGroupKey::None)
      {
        if (!sectionDescriptorsEqual(oldSections, sections))
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

    void insertEntries(std::span<TrackId const> ids)
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
        if (auto const optView = storageValueOrNullopt(reader.get(id, loadMode), "Failed to insert projected tracks");
            optView)
        {
          sortedNew.push_back(buildOrderEntry(id, *optView, dict));
        }
      }

      if (sortedNew.empty())
      {
        return;
      }

      if (groupBy != TrackGroupKey::None)
      {
        auto const oldSections = sections;
        auto insertPositions = mergeEntriesAndCollectPositions(sortedNew);

        if (sectionDescriptorsEqual(oldSections, sections))
        {
          auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
          appendAscendingRanges(batch.deltas,
                                insertPositions,
                                [](Range range) { return TrackListProjectionDelta{ProjectionInsertRange{range}}; });
          publishDelta(batch);
          return;
        }

        publishDelta(TrackListProjectionDeltaBatch{
          .revision = ++rev,
          .deltas = {ProjectionReset{}},
        });
        return;
      }

      mergeEntries(sortedNew);

      // For now, always use ProjectionReset for multiple insertions to simplify UI sync.
      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionReset{}},
      });
    }

    void removeEntry(TrackId trackId)
    {
      auto const optPos = findPosition(trackId);

      if (!optPos)
      {
        return;
      }

      std::size_t const pos = *optPos;
      auto const oldSections = sections;
      eraseEntryAt(pos);

      if (groupBy != TrackGroupKey::None)
      {
        if (!sectionDescriptorsEqual(oldSections, sections))
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

    void removeEntries(std::span<TrackId const> ids)
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

      for (auto const id : ids)
      {
        if (auto optPos = findPosition(id); optPos)
        {
          positions.push_back(*optPos);
        }
      }

      if (positions.empty())
      {
        return;
      }

      auto const oldSections = sections;
      eraseEntriesAtPositions(positions);

      if (groupBy != TrackGroupKey::None)
      {
        if (sectionDescriptorsEqual(oldSections, sections))
        {
          auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
          appendRemovalRangesDescending(batch.deltas, positions);
          publishDelta(batch);
          return;
        }
      }

      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionReset{}},
      });
    }

    void updateEntry(TrackId trackId)
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

      auto const optView = storageValueOrNullopt(reader.get(trackId, loadMode), "Failed to update projected track");

      if (!optView)
      {
        return;
      }

      if (groupBy != TrackGroupKey::None)
      {
        auto newEntry = buildOrderEntry(trackId, *optView, dict);

        if (auto const oldSections = sections;
            comparator && comparator(oldEntry, newEntry) != comparator(newEntry, oldEntry))
        {
          eraseEntryAt(oldPos);

          if (auto const newPos = insertBuiltEntry(std::move(newEntry)); sectionDescriptorsEqual(oldSections, sections))
          {
            publishDelta(TrackListProjectionDeltaBatch{
              .revision = ++rev,
              .deltas =
                {
                  ProjectionRemoveRange{Range{.start = oldPos, .count = 1}},
                  ProjectionInsertRange{Range{.start = newPos, .count = 1}},
                },
            });
            return;
          }
        }
        else
        {
          updateGroupSectionRow(oldPos, std::move(newEntry));

          if (sectionDescriptorsEqual(oldSections, sections))
          {
            publishDelta(TrackListProjectionDeltaBatch{
              .revision = ++rev,
              .deltas = {ProjectionUpdateRange{Range{.start = oldPos, .count = 1}}},
            });
            return;
          }
        }

        publishDelta(TrackListProjectionDeltaBatch{
          .revision = ++rev,
          .deltas = {ProjectionReset{}},
        });
        return;
      }

      auto newKeys = SortKeys{};
      fillSortKeys(newKeys, *optView, dict, sortBy, normCache, stringArena, normScratch);

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

    void updateGroupedEntries(std::span<TrackId const> ids)
    {
      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto processed = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
      processed.reserve(ids.size());

      auto movedEntries = std::vector<PendingMovedEntry>{};
      movedEntries.reserve(ids.size());
      auto updatePositions = std::vector<std::size_t>{};
      updatePositions.reserve(ids.size());
      auto const oldSections = sections;

      for (auto const id : ids)
      {
        if (!processed.insert(id).second)
        {
          continue;
        }

        auto const optPos = findPosition(id);

        if (!optPos)
        {
          continue;
        }

        auto const optView =
          storageValueOrNullopt(reader.get(id, loadMode), "Failed to update grouped projected tracks");

        if (!optView)
        {
          continue;
        }

        auto newEntry = buildOrderEntry(id, *optView, dict);

        if (auto& oldEntry = orderIndex[*optPos];
            comparator && comparator(oldEntry, newEntry) != comparator(newEntry, oldEntry))
        {
          movedEntries.push_back(PendingMovedEntry{.oldPos = *optPos, .entry = std::move(newEntry)});
          continue;
        }

        updateGroupSectionRow(*optPos, std::move(newEntry));
        updatePositions.push_back(*optPos);
      }

      if (updatePositions.empty() && movedEntries.empty())
      {
        return;
      }

      if (!movedEntries.empty())
      {
        auto removePositions = std::vector<std::size_t>{};
        removePositions.reserve(movedEntries.size());

        auto sortedNew = std::vector<OrderEntry>{};
        sortedNew.reserve(movedEntries.size());

        for (auto& moved : movedEntries)
        {
          removePositions.push_back(moved.oldPos);
          sortedNew.push_back(std::move(moved.entry));
        }

        eraseEntriesAtPositions(removePositions);
        auto insertPositions = mergeEntriesAndCollectPositions(sortedNew);

        if (updatePositions.empty() && sectionDescriptorsEqual(oldSections, sections))
        {
          auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
          appendRemovalRangesDescending(batch.deltas, removePositions);
          appendAscendingRanges(batch.deltas,
                                insertPositions,
                                [](Range range) { return TrackListProjectionDelta{ProjectionInsertRange{range}}; });
          publishDelta(batch);
          return;
        }
      }

      if (movedEntries.empty() && sectionDescriptorsEqual(oldSections, sections))
      {
        auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
        appendAscendingRanges(batch.deltas,
                              updatePositions,
                              [](Range range) { return TrackListProjectionDelta{ProjectionUpdateRange{range}}; });
        publishDelta(batch);
        return;
      }

      publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionReset{}},
      });
    }

    void applyUngroupedMovedEntries(std::vector<PendingMovedEntry>& movedEntries,
                                    bool publishReset,
                                    TrackListProjectionDeltaBatch& batch)
    {
      auto removePositions = std::vector<std::size_t>{};
      removePositions.reserve(movedEntries.size());

      auto sortedNew = std::vector<OrderEntry>{};
      sortedNew.reserve(movedEntries.size());

      for (auto& moved : movedEntries)
      {
        removePositions.push_back(moved.oldPos);
        sortedNew.push_back(std::move(moved.entry));
      }

      if (publishReset)
      {
        sortUniquePositions(removePositions);
        std::ranges::reverse(removePositions);
      }
      else
      {
        appendRemovalRangesDescending(batch.deltas, removePositions);
      }

      for (auto const pos : removePositions)
      {
        orderIndex.erase(orderIndex.begin() + static_cast<std::ptrdiff_t>(pos));
      }

      std::ranges::sort(sortedNew, std::ref(comparator));

      auto merged = std::vector<OrderEntry>{};
      merged.reserve(orderIndex.size() + sortedNew.size());
      std::ranges::merge(orderIndex, sortedNew, std::back_inserter(merged), std::ref(comparator));

      orderIndex = std::move(merged);
      rebuildPositionIndex();

      auto insertPositions = std::vector<std::size_t>{};
      insertPositions.reserve(sortedNew.size());

      for (auto const& entry : sortedNew)
      {
        if (auto const optPos = findPosition(entry.trackId); optPos)
        {
          insertPositions.push_back(*optPos);
        }
      }

      if (!publishReset)
      {
        appendAscendingRanges(batch.deltas,
                              insertPositions,
                              [](Range range) { return TrackListProjectionDelta{ProjectionInsertRange{range}}; });
      }
    }

    void updateUngroupedEntries(std::span<TrackId const> ids)
    {
      auto const txn = library.readTransaction();
      auto const reader = library.tracks().reader(txn);
      auto& dict = library.dictionary();

      auto processed = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
      processed.reserve(ids.size());

      auto updatePositions = std::vector<std::size_t>{};
      updatePositions.reserve(ids.size());

      auto movedEntries = std::vector<PendingMovedEntry>{};
      movedEntries.reserve(ids.size());

      for (auto const id : ids)
      {
        if (!processed.insert(id).second)
        {
          continue;
        }

        auto const optPos = findPosition(id);

        if (!optPos)
        {
          continue;
        }

        auto const optView = storageValueOrNullopt(reader.get(id, loadMode), "Failed to update projected tracks");

        if (!optView)
        {
          continue;
        }

        auto newKeys = SortKeys{};
        fillSortKeys(newKeys, *optView, dict, sortBy, normCache, stringArena, normScratch);

        auto& oldEntry = orderIndex[*optPos];

        if (!comparator)
        {
          oldEntry.keys = newKeys;
          updatePositions.push_back(*optPos);
          continue;
        }

        auto testEntry = OrderEntry{.trackId = id, .keys = newKeys};

        if (comparator(oldEntry, testEntry) == comparator(testEntry, oldEntry))
        {
          oldEntry.keys = newKeys;
          updatePositions.push_back(*optPos);
          continue;
        }

        movedEntries.push_back(PendingMovedEntry{.oldPos = *optPos, .entry = std::move(testEntry)});
      }

      if (updatePositions.empty() && movedEntries.empty())
      {
        return;
      }

      auto const publishReset = !updatePositions.empty() && !movedEntries.empty();
      auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};

      if (!publishReset)
      {
        appendAscendingRanges(batch.deltas,
                              updatePositions,
                              [](Range range) { return TrackListProjectionDelta{ProjectionUpdateRange{range}}; });
      }

      if (!movedEntries.empty())
      {
        applyUngroupedMovedEntries(movedEntries, publishReset, batch);
      }

      if (publishReset)
      {
        batch.deltas = {ProjectionReset{}};
      }

      publishDelta(batch);
    }

    void updateEntries(std::span<TrackId const> ids)
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

      if (groupBy != TrackGroupKey::None)
      {
        updateGroupedEntries(ids);
        return;
      }

      updateUngroupedEntries(ids);
    }
  };

  TrackListProjection::TrackListProjection(ViewId viewId, TrackSource& source, library::MusicLibrary& library)
    : _implPtr{std::make_unique<Impl>(viewId, source, library)}
  {
    source.attach(this);
  }

  TrackListProjection::~TrackListProjection()
  {
    if (!_implPtr->sourceDestroyed)
    {
      _implPtr->source.detach(this);
    }
  }

  ViewId TrackListProjection::viewId() const noexcept
  {
    return _implPtr->viewId;
  }

  std::uint64_t TrackListProjection::revision() const noexcept
  {
    return _implPtr->rev;
  }

  void TrackListProjection::setPresentation(TrackPresentationSpec const& presentation)
  {
    auto spec = normalizeTrackPresentationSpec(presentation);

    _implPtr->id = spec.id;
    _implPtr->visibleFields = spec.visibleFields;
    _implPtr->redundantFields = spec.redundantFields;

    // Fall back to the built-in preset when redundant fields are unspecified.
    if (_implPtr->redundantFields.empty() && spec.groupBy != TrackGroupKey::None)
    {
      for (auto const& preset : builtinTrackPresentationPresets())
      {
        if (preset.spec.groupBy == spec.groupBy)
        {
          _implPtr->redundantFields = preset.spec.redundantFields;
          break;
        }
      }
    }

    // Same-group / reverse-sort fast path.
    if (_implPtr->groupBy == spec.groupBy && sameSortDirection(_implPtr->sortBy, spec.sortBy) && _implPtr->comparator)
    {
      _implPtr->sortBy = std::move(spec.sortBy);
      _implPtr->comparator = buildComparator(_implPtr->sortBy);
      std::ranges::reverse(_implPtr->orderIndex);
      _implPtr->rebuildPositionIndex();

      _implPtr->publishDelta(TrackListProjectionDeltaBatch{
        .revision = ++_implPtr->rev,
        .deltas = {ProjectionReset{}},
      });
      return;
    }

    _implPtr->groupBy = spec.groupBy;
    _implPtr->sortBy = std::move(spec.sortBy);
    _implPtr->comparator = buildComparator(_implPtr->sortBy);
    _implPtr->loadMode = computeLoadMode(_implPtr->sortBy, _implPtr->groupBy);

    _implPtr->rebuildOrderIndex();

    _implPtr->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_implPtr->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  std::size_t TrackListProjection::size() const noexcept
  {
    return _implPtr->orderIndex.size();
  }

  TrackId TrackListProjection::trackIdAt(std::size_t index) const
  {
    if (index >= _implPtr->orderIndex.size())
    {
      return kInvalidTrackId;
    }

    return _implPtr->orderIndex[index].trackId;
  }

  std::optional<std::size_t> TrackListProjection::indexOf(TrackId trackId) const noexcept
  {
    if (auto const it = _implPtr->positionIndex.find(trackId); it != _implPtr->positionIndex.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  TrackPresentationSpec TrackListProjection::presentation() const
  {
    return TrackPresentationSpec{
      .id = _implPtr->id,
      .groupBy = _implPtr->groupBy,
      .sortBy = _implPtr->sortBy,
      .visibleFields = _implPtr->visibleFields,
      .redundantFields = _implPtr->redundantFields,
    };
  }

  std::size_t TrackListProjection::groupCount() const noexcept
  {
    return _implPtr->sections.size();
  }

  TrackGroupSectionSnapshot TrackListProjection::groupAt(std::size_t groupIndex) const
  {
    if (groupIndex >= _implPtr->sections.size())
    {
      return {};
    }

    auto const& section = _implPtr->sections[groupIndex];
    return TrackGroupSectionSnapshot{
      .rows = section.rows,
      .primaryText = (section.primaryText.data() != nullptr) ? std::string{section.primaryText} : std::string{},
      .secondaryText = (section.secondaryText.data() != nullptr) ? std::string{section.secondaryText} : std::string{},
      .tertiaryText = (section.tertiaryText.data() != nullptr) ? std::string{section.tertiaryText} : std::string{},
      .imageId = section.imageId,
    };
  }

  std::optional<std::size_t> TrackListProjection::groupIndexAt(std::size_t rowIndex) const
  {
    return _implPtr->findSectionIndexAt(rowIndex);
  }

  Subscription TrackListProjection::subscribe(
    std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler)
  {
    handler(TrackListProjectionDeltaBatch{
      .revision = _implPtr->rev,
      .deltas = {ProjectionReset{}},
    });

    _implPtr->subscribers.push_back(std::move(handler));
    std::size_t const idx = _implPtr->subscribers.size() - 1;

    return Subscription{[this, idx] { _implPtr->subscribers[idx] = {}; }};
  }

  void TrackListProjection::onReset()
  {
    _implPtr->rebuildOrderIndex();
    _implPtr->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_implPtr->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  void TrackListProjection::onInserted(TrackId id, std::size_t /*sourceIndex*/)
  {
    _implPtr->insertEntry(id);
  }

  void TrackListProjection::onUpdated(TrackId id, std::size_t /*sourceIndex*/)
  {
    _implPtr->updateEntry(id);
  }

  void TrackListProjection::onRemoved(TrackId id, std::size_t /*sourceIndex*/)
  {
    _implPtr->removeEntry(id);
  }

  void TrackListProjection::onBulkInserted(std::span<TrackId const> ids)
  {
    _implPtr->insertEntries(ids);
  }

  void TrackListProjection::onBulkUpdated(std::span<TrackId const> ids)
  {
    _implPtr->updateEntries(ids);
  }

  void TrackListProjection::onBulkRemoved(std::span<TrackId const> ids)
  {
    _implPtr->removeEntries(ids);
  }

  void TrackListProjection::publishDelta(TrackListProjectionDeltaBatch const& batch)
  {
    _implPtr->publishDelta(batch);
  }

  void TrackListProjection::onSourceDestroyed()
  {
    _implPtr->sourceDestroyed = true;
  }
} // namespace ao::rt
