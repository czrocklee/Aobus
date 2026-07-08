// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
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
      std::string_view conductorKey{};
      std::string_view ensembleKey{};
      std::string_view workKey{};
      std::string_view soloistKey{};
    };

    struct GroupSection final
    {
      TrackRowRange rows{};
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

    void sortUniqueRowIndices(std::vector<std::size_t>& rowIndices)
    {
      std::ranges::sort(rowIndices);
      rowIndices.erase(std::ranges::unique(rowIndices).begin(), rowIndices.end());
    }

    void appendAscendingRanges(boost::container::small_vector<TrackListProjectionDelta, 1>& deltas,
                               std::vector<std::size_t>& rowIndices,
                               auto makeDelta)
    {
      if (rowIndices.empty())
      {
        return;
      }

      sortUniqueRowIndices(rowIndices);

      auto start = rowIndices.front();
      auto previousRowIndex = start;

      for (auto const rowIndex : rowIndices | std::views::drop(1))
      {
        if (rowIndex == previousRowIndex + 1)
        {
          previousRowIndex = rowIndex;
          continue;
        }

        deltas.push_back(makeDelta(TrackRowRange{.start = start, .count = previousRowIndex - start + 1}));
        start = rowIndex;
        previousRowIndex = rowIndex;
      }

      deltas.push_back(makeDelta(TrackRowRange{.start = start, .count = previousRowIndex - start + 1}));
    }

    void appendRemovalRangesDescending(boost::container::small_vector<TrackListProjectionDelta, 1>& deltas,
                                       std::vector<std::size_t>& rowIndices)
    {
      if (rowIndices.empty())
      {
        return;
      }

      sortUniqueRowIndices(rowIndices);
      std::ranges::reverse(rowIndices);

      auto high = rowIndices.front();
      auto low = high;

      for (auto const rowIndex : rowIndices | std::views::drop(1))
      {
        if (rowIndex + 1 == low)
        {
          low = rowIndex;
          continue;
        }

        deltas.push_back(ProjectionRemoveRange{TrackRowRange{.start = low, .count = high - low + 1}});
        high = rowIndex;
        low = rowIndex;
      }

      deltas.push_back(ProjectionRemoveRange{TrackRowRange{.start = low, .count = high - low + 1}});
    }

    bool startsWithCaseInsensitive(std::string_view str, std::string_view prefix)
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

      if (startsWithCaseInsensitive(title, "the "))
      {
        offset = 4;
      }
      else if (startsWithCaseInsensitive(title, "a "))
      {
        offset = 2;
      }
      else if (startsWithCaseInsensitive(title, "an "))
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

    bool needsColdDataForSortField(TrackSortField field)
    {
      switch (field)
      {
        case TrackSortField::Duration:
        case TrackSortField::DiscNumber:
        case TrackSortField::TrackNumber:
        case TrackSortField::Conductor:
        case TrackSortField::Ensemble:
        case TrackSortField::Work:
        case TrackSortField::Movement:
        case TrackSortField::Soloist: return true;
        default: return false;
      }
    }

    bool needsColdDataForGroupBy(TrackGroupKey groupBy)
    {
      return groupBy == TrackGroupKey::Work || groupBy == TrackGroupKey::Album || groupBy == TrackGroupKey::Conductor ||
             groupBy == TrackGroupKey::Ensemble;
    }

    library::TrackStore::Reader::LoadMode computeLoadMode(std::vector<TrackSortTerm> const& sortBy,
                                                          TrackGroupKey groupBy)
    {
      bool needsHot = groupBy != TrackGroupKey::None;
      bool needsCold = needsColdDataForGroupBy(groupBy);

      for (auto const& term : sortBy)
      {
        if (needsColdDataForSortField(term.field))
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
        case TrackSortField::Conductor: return lhs.conductorKey.compare(rhs.conductorKey);
        case TrackSortField::Ensemble: return lhs.ensembleKey.compare(rhs.ensembleKey);
        case TrackSortField::Work: return lhs.workKey.compare(rhs.workKey);
        case TrackSortField::Soloist: return lhs.soloistKey.compare(rhs.soloistKey);
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
                                     library::DictionaryStore& dictionary,
                                     DictionaryId id)
    {
      if (auto const it = normCache.find(id); it != normCache.end())
      {
        return it->second;
      }

      normalizeInto(scratch, dictionary.getOrDefault(id));
      auto const view = arena.intern(scratch);
      normCache.emplace(id, view);
      return view;
    }

    void fillSortKeys(SortKeys& keys,
                      library::TrackView const& view,
                      library::DictionaryStore& dictionary,
                      std::vector<TrackSortTerm> const& sortBy,
                      NormCache& normCache,
                      utility::StringArena& arena,
                      std::string& scratch)
    {
      auto const normalizedText = [&](DictionaryId id) -> std::string_view
      { return normalizeCached(normCache, arena, scratch, dictionary, id); };

      for (auto const& term : sortBy)
      {
        switch (term.field)
        {
          case TrackSortField::Year: keys.year = view.metadata().year(); break;
          case TrackSortField::DiscNumber: keys.discNumber = view.metadata().discNumber(); break;
          case TrackSortField::TrackNumber: keys.trackNumber = view.metadata().trackNumber(); break;
          case TrackSortField::Movement: keys.movementNumber = view.classical().movementNumber(); break;
          case TrackSortField::Duration: keys.duration = view.property().duration(); break;
          case TrackSortField::Title:
            normalizeInto(scratch, view.metadata().title());
            keys.titleKey = arena.intern(scratch);
            break;
          case TrackSortField::Artist: keys.artistKey = normalizedText(view.metadata().artistId()); break;
          case TrackSortField::Album: keys.albumKey = normalizedText(view.metadata().albumId()); break;
          case TrackSortField::AlbumArtist:
            keys.albumArtistKey = normalizedText(view.metadata().albumArtistId());
            break;
          case TrackSortField::Genre: keys.genreKey = normalizedText(view.metadata().genreId()); break;
          case TrackSortField::Composer: keys.composerKey = normalizedText(view.metadata().composerId()); break;
          case TrackSortField::Conductor: keys.conductorKey = normalizedText(view.classical().conductorId()); break;
          case TrackSortField::Ensemble: keys.ensembleKey = normalizedText(view.classical().ensembleId()); break;
          case TrackSortField::Work: keys.workKey = normalizedText(view.classical().workId()); break;
          case TrackSortField::Soloist: keys.soloistKey = normalizedText(view.classical().soloistId()); break;
        }
      }
    }

    void ensureGroupSortKeys(SortKeys& keys,
                             library::TrackView const& view,
                             library::DictionaryStore& dictionary,
                             TrackGroupKey groupBy,
                             NormCache& normCache,
                             utility::StringArena& arena,
                             std::string& scratch)
    {
      auto const normalizedText = [&](DictionaryId id) -> std::string_view
      { return normalizeCached(normCache, arena, scratch, dictionary, id); };

      switch (groupBy)
      {
        case TrackGroupKey::Artist:
          if (keys.artistKey.empty())
          {
            keys.artistKey = normalizedText(view.metadata().artistId());
          }

          break;
        case TrackGroupKey::Album:
          if (keys.albumKey.empty())
          {
            keys.albumKey = normalizedText(view.metadata().albumId());
          }

          if (keys.albumArtistKey.empty())
          {
            keys.albumArtistKey = normalizedText(view.metadata().albumArtistId());
          }

          break;
        case TrackGroupKey::AlbumArtist:
          if (keys.albumArtistKey.empty())
          {
            keys.albumArtistKey = normalizedText(view.metadata().albumArtistId());
          }

          break;
        case TrackGroupKey::Genre:
          if (keys.genreKey.empty())
          {
            keys.genreKey = normalizedText(view.metadata().genreId());
          }

          break;
        case TrackGroupKey::Composer:
          if (keys.composerKey.empty())
          {
            keys.composerKey = normalizedText(view.metadata().composerId());
          }

          break;
        case TrackGroupKey::Conductor:
          if (keys.conductorKey.empty())
          {
            keys.conductorKey = normalizedText(view.classical().conductorId());
          }

          break;
        case TrackGroupKey::Ensemble:
          if (keys.ensembleKey.empty())
          {
            keys.ensembleKey = normalizedText(view.classical().ensembleId());
          }

          break;
        case TrackGroupKey::Work:
          if (keys.workKey.empty())
          {
            keys.workKey = normalizedText(view.classical().workId());
          }

          if (keys.composerKey.empty())
          {
            keys.composerKey = normalizedText(view.metadata().composerId());
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
                           library::DictionaryStore& dictionary,
                           TrackGroupKey groupBy,
                           utility::StringArena& arena,
                           std::string& scratch)
    {
      switch (groupBy)
      {
        case TrackGroupKey::None: return;
        case TrackGroupKey::Artist:
          entry.groupKey = entry.keys.artistKey;
          entry.primaryText = dictionary.getOrDefault(view.metadata().artistId(), "Unknown Artist");
          break;
        case TrackGroupKey::Album:
          entry.groupKey = internCompoundKey(arena, scratch, entry.keys.albumArtistKey, entry.keys.albumKey);

          if (auto const optPrimary = view.coverArt().primary(); optPrimary)
          {
            entry.imageId = optPrimary->resourceId;
          }

          {
            auto album = std::string{dictionary.getOrDefault(view.metadata().albumId())};
            auto albumArtist = std::string{dictionary.getOrDefault(view.metadata().albumArtistId())};

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
          entry.primaryText = dictionary.getOrDefault(view.metadata().albumArtistId(), "Unknown Artist");
          break;
        case TrackGroupKey::Genre:
          entry.groupKey = entry.keys.genreKey;
          entry.primaryText = dictionary.getOrDefault(view.metadata().genreId(), "Unknown Genre");
          break;
        case TrackGroupKey::Composer:
          entry.groupKey = entry.keys.composerKey;
          entry.primaryText = dictionary.getOrDefault(view.metadata().composerId(), "Unknown Composer");
          break;
        case TrackGroupKey::Conductor:
          entry.groupKey = entry.keys.conductorKey;
          entry.primaryText = dictionary.getOrDefault(view.classical().conductorId(), "Unknown Conductor");
          break;
        case TrackGroupKey::Ensemble:
          entry.groupKey = entry.keys.ensembleKey;
          entry.primaryText = dictionary.getOrDefault(view.classical().ensembleId(), "Unknown Ensemble");
          break;
        case TrackGroupKey::Work:
          entry.groupKey = internCompoundKey(arena, scratch, entry.keys.composerKey, entry.keys.workKey);
          entry.primaryText = dictionary.getOrDefault(view.classical().workId(), "Unknown Work");
          entry.secondaryText = dictionary.getOrDefault(view.metadata().composerId(), "Unknown Composer");
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

    bool hasSameSortDirection(std::vector<TrackSortTerm> const& old, std::vector<TrackSortTerm> const& updated)
    {
      if (old.size() != updated.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < old.size(); ++index)
      {
        if (old[index].field != updated[index].field)
        {
          return false;
        }
      }

      return true;
    }
  } // namespace

  struct LiveTrackListProjection::Impl final
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
    boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>> rowIndexByTrackId;
    std::vector<GroupSection> sections;
    std::uint64_t rev = 0;
    std::vector<std::move_only_function<void(TrackListProjectionDeltaBatch const&)>> subscribers;
    bool sourceDestroyed = false;

    struct PendingMovedEntry final
    {
      std::size_t oldRowIndex = 0;
      OrderEntry entry;
    };

    OrderEntry buildOrderEntry(TrackId id, library::TrackView const& view, library::DictionaryStore& dictionary)
    {
      auto entry = OrderEntry{.trackId = id};
      fillSortKeys(entry.keys, view, dictionary, sortBy, normCache, stringArena, normScratch);

      if (groupBy != TrackGroupKey::None)
      {
        ensureGroupSortKeys(entry.keys, view, dictionary, groupBy, normCache, stringArena, normScratch);
        fillGroupMetadata(entry, view, dictionary, groupBy, stringArena, normScratch);
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

      rebuildRowIndex();
    }

    std::vector<std::size_t> mergeEntriesAndCollectRowIndices(std::vector<OrderEntry>& entries)
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

      auto rowIndices = std::vector<std::size_t>{};
      rowIndices.reserve(ids.size());

      for (auto const id : ids)
      {
        if (auto const optRowIndex = findRowIndex(id); optRowIndex)
        {
          rowIndices.push_back(*optRowIndex);
        }
      }

      return rowIndices;
    }

    void rebuildGroups()
    {
      if (groupBy == TrackGroupKey::None || orderIndex.empty())
      {
        sections.clear();
        return;
      }

      auto transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      for (auto& entry : orderIndex)
      {
        if (auto const optView =
              storageValueOrNullopt(reader.get(entry.trackId, loadMode), "Failed to rebuild track groups");
            optView)
        {
          ensureGroupSortKeys(entry.keys, *optView, dictionary, groupBy, normCache, stringArena, normScratch);
          fillGroupMetadata(entry, *optView, dictionary, groupBy, stringArena, normScratch);
        }
      }
    }

    Impl(ViewId vid, TrackSource& trackSource, library::MusicLibrary& lib)
      : viewId{vid}, source{trackSource}, library{lib}
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

      for (std::size_t index = 1; index < orderIndex.size(); ++index)
      {
        if (orderIndex[index].groupKey != orderIndex[index - 1].groupKey)
        {
          sections.push_back(GroupSection{
            .rows = {.start = index, .count = 1},
            .groupKey = orderIndex[index].groupKey,
            .primaryText = orderIndex[index].primaryText,
            .secondaryText = orderIndex[index].secondaryText,
            .tertiaryText = orderIndex[index].tertiaryText,
            .imageId = orderIndex[index].imageId,
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
      auto const timer = rt::ScopedTimer{"LiveTrackListProjection::rebuildOrderIndex"};
      orderIndex.clear();
      rowIndexByTrackId.clear();
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

      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      for (std::size_t index = 0; index < source.size(); ++index)
      {
        auto const trackId = source.trackIdAt(index);

        if (auto const optView = storageValueOrNullopt(reader.get(trackId, loadMode), "Failed to rebuild track order");
            optView)
        {
          orderIndex.push_back(buildOrderEntry(trackId, *optView, dictionary));
        }
      }

      if (comparator)
      {
        std::ranges::sort(orderIndex, std::ref(comparator));
      }

      rebuildRowIndex();
      buildGroupSections();
    }

    void rebuildRowIndex()
    {
      rowIndexByTrackId.clear();
      rowIndexByTrackId.reserve(orderIndex.size());

      for (auto const& [index, entry] : std::ranges::views::enumerate(orderIndex))
      {
        rowIndexByTrackId[entry.trackId] = static_cast<std::size_t>(index);
      }
    }

    std::optional<std::size_t> findRowIndex(TrackId trackId) const
    {
      if (auto it = rowIndexByTrackId.find(trackId); it != rowIndexByTrackId.end())
      {
        return it->second;
      }

      return std::nullopt;
    }

    bool hasSameSectionDescriptors(std::vector<GroupSection> const& left, std::vector<GroupSection> const& right) const
    {
      if (left.size() != right.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < left.size(); ++index)
      {
        if (left[index].groupKey != right[index].groupKey || left[index].primaryText != right[index].primaryText ||
            left[index].secondaryText != right[index].secondaryText ||
            left[index].tertiaryText != right[index].tertiaryText || left[index].imageId != right[index].imageId)
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
      auto const index = static_cast<std::size_t>(it - sections.begin());

      if (auto const& section = sections[index];
          row >= section.rows.start && row < section.rows.start + section.rows.count)
      {
        return index;
      }

      return std::nullopt;
    }

    std::size_t sectionInsertionIndex(std::size_t row) const
    {
      auto it =
        std::ranges::upper_bound(sections, row, {}, [](GroupSection const& section) { return section.rows.start; });
      return static_cast<std::size_t>(it - sections.begin());
    }

    void insertGroupSectionRow(std::size_t rowIndex)
    {
      if (groupBy == TrackGroupKey::None)
      {
        return;
      }

      auto const& entry = orderIndex[rowIndex];
      auto const leftSame = rowIndex > 0 && orderIndex[rowIndex - 1].groupKey == entry.groupKey;
      auto const rightSame = rowIndex + 1 < orderIndex.size() && orderIndex[rowIndex + 1].groupKey == entry.groupKey;
      auto const optLeftSection = leftSame ? findSectionIndexAt(rowIndex - 1) : std::optional<std::size_t>{};
      auto const optRightSection = rightSame ? findSectionIndexAt(rowIndex) : std::optional<std::size_t>{};

      for (auto& section : sections)
      {
        if (section.rows.start >= rowIndex)
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
        section.rows.start = rowIndex;
        ++section.rows.count;
        assignSectionMetadata(section, entry);
        return;
      }

      auto const insertAt = sectionInsertionIndex(rowIndex);
      sections.insert(sections.begin() + static_cast<std::ptrdiff_t>(insertAt), makeSection(rowIndex, entry));
    }

    void removeGroupSectionRow(std::size_t rowIndex)
    {
      if (groupBy == TrackGroupKey::None)
      {
        return;
      }

      auto const optSection = findSectionIndexAt(rowIndex);

      if (!optSection)
      {
        return;
      }

      auto const sectionIndex = *optSection;
      auto& section = sections[sectionIndex];
      auto shiftFrom = sectionIndex + 1;

      if (section.rows.count == 1)
      {
        sections.erase(sections.begin() + static_cast<std::ptrdiff_t>(sectionIndex));
        shiftFrom = sectionIndex;
      }
      else
      {
        if (section.rows.start == rowIndex)
        {
          assignSectionMetadata(section, orderIndex[rowIndex + 1]);
        }

        --section.rows.count;
      }

      for (std::size_t index = shiftFrom; index < sections.size(); ++index)
      {
        --sections[index].rows.start;
      }
    }

    void updateGroupSectionRow(std::size_t rowIndex, OrderEntry entry)
    {
      if (groupBy == TrackGroupKey::None)
      {
        orderIndex[rowIndex] = std::move(entry);
        return;
      }

      if (auto const oldGroupKey = orderIndex[rowIndex].groupKey; oldGroupKey == entry.groupKey)
      {
        orderIndex[rowIndex] = std::move(entry);

        if (auto const optSection = findSectionIndexAt(rowIndex);
            optSection && sections[*optSection].rows.start == rowIndex)
        {
          assignSectionMetadata(sections[*optSection], orderIndex[rowIndex]);
        }

        return;
      }

      removeGroupSectionRow(rowIndex);
      orderIndex[rowIndex] = std::move(entry);
      insertGroupSectionRow(rowIndex);
    }

    std::size_t insertBuiltEntry(OrderEntry entry)
    {
      std::size_t rowIndex = 0;

      if (comparator)
      {
        auto it = std::ranges::lower_bound(orderIndex, entry, std::ref(comparator));
        rowIndex = static_cast<std::size_t>(it - orderIndex.begin());
        orderIndex.insert(it, std::move(entry));
      }
      else
      {
        rowIndex = orderIndex.size();
        orderIndex.push_back(std::move(entry));
      }

      insertGroupSectionRow(rowIndex);

      for (std::size_t index = rowIndex; index < orderIndex.size(); ++index)
      {
        rowIndexByTrackId[orderIndex[index].trackId] = index;
      }

      return rowIndex;
    }

    void eraseEntryAt(std::size_t rowIndex)
    {
      auto const id = orderIndex[rowIndex].trackId;
      removeGroupSectionRow(rowIndex);
      orderIndex.erase(orderIndex.begin() + static_cast<std::ptrdiff_t>(rowIndex));
      rowIndexByTrackId.erase(id);

      for (std::size_t index = rowIndex; index < orderIndex.size(); ++index)
      {
        rowIndexByTrackId[orderIndex[index].trackId] = index;
      }
    }

    void eraseEntriesAtRowIndices(std::vector<std::size_t>& rowIndices)
    {
      if (rowIndices.empty())
      {
        return;
      }

      sortUniqueRowIndices(rowIndices);

      auto retained = std::vector<OrderEntry>{};
      retained.reserve(orderIndex.size() - rowIndices.size());

      std::size_t removeIndex = 0;

      for (std::size_t index = 0; index < orderIndex.size(); ++index)
      {
        if (removeIndex < rowIndices.size() && rowIndices[removeIndex] == index)
        {
          ++removeIndex;
          continue;
        }

        retained.push_back(std::move(orderIndex[index]));
      }

      orderIndex = std::move(retained);
      rebuildRowIndex();

      if (groupBy != TrackGroupKey::None)
      {
        buildGroupSections();
      }
    }

    void insertEntry(TrackId trackId)
    {
      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      auto const optView = storageValueOrNullopt(reader.get(trackId, loadMode), "Failed to insert projected track");

      if (!optView)
      {
        return;
      }

      auto entry = buildOrderEntry(trackId, *optView, dictionary);
      auto const oldSections = sections;
      auto const rowIndex = insertBuiltEntry(std::move(entry));

      if (groupBy != TrackGroupKey::None)
      {
        if (!hasSameSectionDescriptors(oldSections, sections))
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
        .deltas = {ProjectionInsertRange{TrackRowRange{.start = rowIndex, .count = 1}}},
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

      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      auto sortedNew = std::vector<OrderEntry>{};
      sortedNew.reserve(ids.size());

      for (auto const id : ids)
      {
        if (auto const optView = storageValueOrNullopt(reader.get(id, loadMode), "Failed to insert projected tracks");
            optView)
        {
          sortedNew.push_back(buildOrderEntry(id, *optView, dictionary));
        }
      }

      if (sortedNew.empty())
      {
        return;
      }

      if (groupBy != TrackGroupKey::None)
      {
        auto const oldSections = sections;
        auto insertRowIndices = mergeEntriesAndCollectRowIndices(sortedNew);

        if (hasSameSectionDescriptors(oldSections, sections))
        {
          auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
          appendAscendingRanges(batch.deltas,
                                insertRowIndices,
                                [](TrackRowRange range)
                                { return TrackListProjectionDelta{ProjectionInsertRange{range}}; });
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
      auto const optRowIndex = findRowIndex(trackId);

      if (!optRowIndex)
      {
        return;
      }

      std::size_t const rowIndex = *optRowIndex;
      auto const oldSections = sections;
      eraseEntryAt(rowIndex);

      if (groupBy != TrackGroupKey::None)
      {
        if (!hasSameSectionDescriptors(oldSections, sections))
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
        .deltas = {ProjectionRemoveRange{TrackRowRange{.start = rowIndex, .count = 1}}},
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

      auto rowIndices = std::vector<std::size_t>{};
      rowIndices.reserve(ids.size());

      for (auto const id : ids)
      {
        if (auto optRowIndex = findRowIndex(id); optRowIndex)
        {
          rowIndices.push_back(*optRowIndex);
        }
      }

      if (rowIndices.empty())
      {
        return;
      }

      auto const oldSections = sections;
      eraseEntriesAtRowIndices(rowIndices);

      if (groupBy != TrackGroupKey::None)
      {
        if (hasSameSectionDescriptors(oldSections, sections))
        {
          auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
          appendRemovalRangesDescending(batch.deltas, rowIndices);
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
      auto optRowIndex = findRowIndex(trackId);

      if (!optRowIndex)
      {
        return;
      }

      auto oldRowIndex = *optRowIndex;
      auto& oldEntry = orderIndex[oldRowIndex];

      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      auto const optView = storageValueOrNullopt(reader.get(trackId, loadMode), "Failed to update projected track");

      if (!optView)
      {
        return;
      }

      if (groupBy != TrackGroupKey::None)
      {
        auto newEntry = buildOrderEntry(trackId, *optView, dictionary);

        if (auto const oldSections = sections;
            comparator && comparator(oldEntry, newEntry) != comparator(newEntry, oldEntry))
        {
          eraseEntryAt(oldRowIndex);

          if (auto const newRowIndex = insertBuiltEntry(std::move(newEntry));
              hasSameSectionDescriptors(oldSections, sections))
          {
            publishDelta(TrackListProjectionDeltaBatch{
              .revision = ++rev,
              .deltas =
                {
                  ProjectionRemoveRange{TrackRowRange{.start = oldRowIndex, .count = 1}},
                  ProjectionInsertRange{TrackRowRange{.start = newRowIndex, .count = 1}},
                },
            });
            return;
          }
        }
        else
        {
          updateGroupSectionRow(oldRowIndex, std::move(newEntry));

          if (hasSameSectionDescriptors(oldSections, sections))
          {
            publishDelta(TrackListProjectionDeltaBatch{
              .revision = ++rev,
              .deltas = {ProjectionUpdateRange{TrackRowRange{.start = oldRowIndex, .count = 1}}},
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
      fillSortKeys(newKeys, *optView, dictionary, sortBy, normCache, stringArena, normScratch);

      if (comparator)
      {
        auto testEntry = OrderEntry{.trackId = trackId, .keys = newKeys};

        if (comparator(oldEntry, testEntry) == comparator(testEntry, oldEntry))
        {
          oldEntry.keys = testEntry.keys;
          publishDelta(TrackListProjectionDeltaBatch{
            .revision = ++rev,
            .deltas = {ProjectionUpdateRange{TrackRowRange{.start = oldRowIndex, .count = 1}}},
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
        .deltas = {ProjectionUpdateRange{TrackRowRange{.start = oldRowIndex, .count = 1}}},
      });
    }

    void updateGroupedEntries(std::span<TrackId const> ids)
    {
      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      auto processed = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
      processed.reserve(ids.size());

      auto movedEntries = std::vector<PendingMovedEntry>{};
      movedEntries.reserve(ids.size());
      auto updateRowIndices = std::vector<std::size_t>{};
      updateRowIndices.reserve(ids.size());
      auto const oldSections = sections;

      for (auto const id : ids)
      {
        if (!processed.insert(id).second)
        {
          continue;
        }

        auto const optRowIndex = findRowIndex(id);

        if (!optRowIndex)
        {
          continue;
        }

        auto const optView =
          storageValueOrNullopt(reader.get(id, loadMode), "Failed to update grouped projected tracks");

        if (!optView)
        {
          continue;
        }

        auto newEntry = buildOrderEntry(id, *optView, dictionary);

        if (auto& oldEntry = orderIndex[*optRowIndex];
            comparator && comparator(oldEntry, newEntry) != comparator(newEntry, oldEntry))
        {
          movedEntries.push_back(PendingMovedEntry{.oldRowIndex = *optRowIndex, .entry = std::move(newEntry)});
          continue;
        }

        updateGroupSectionRow(*optRowIndex, std::move(newEntry));
        updateRowIndices.push_back(*optRowIndex);
      }

      if (updateRowIndices.empty() && movedEntries.empty())
      {
        return;
      }

      if (!movedEntries.empty())
      {
        auto removeRowIndices = std::vector<std::size_t>{};
        removeRowIndices.reserve(movedEntries.size());

        auto sortedNew = std::vector<OrderEntry>{};
        sortedNew.reserve(movedEntries.size());

        for (auto& moved : movedEntries)
        {
          removeRowIndices.push_back(moved.oldRowIndex);
          sortedNew.push_back(std::move(moved.entry));
        }

        eraseEntriesAtRowIndices(removeRowIndices);
        auto insertRowIndices = mergeEntriesAndCollectRowIndices(sortedNew);

        if (updateRowIndices.empty() && hasSameSectionDescriptors(oldSections, sections))
        {
          auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
          appendRemovalRangesDescending(batch.deltas, removeRowIndices);
          appendAscendingRanges(batch.deltas,
                                insertRowIndices,
                                [](TrackRowRange range)
                                { return TrackListProjectionDelta{ProjectionInsertRange{range}}; });
          publishDelta(batch);
          return;
        }
      }

      if (movedEntries.empty() && hasSameSectionDescriptors(oldSections, sections))
      {
        auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};
        appendAscendingRanges(batch.deltas,
                              updateRowIndices,
                              [](TrackRowRange range)
                              { return TrackListProjectionDelta{ProjectionUpdateRange{range}}; });
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
      auto removeRowIndices = std::vector<std::size_t>{};
      removeRowIndices.reserve(movedEntries.size());

      auto sortedNew = std::vector<OrderEntry>{};
      sortedNew.reserve(movedEntries.size());

      for (auto& moved : movedEntries)
      {
        removeRowIndices.push_back(moved.oldRowIndex);
        sortedNew.push_back(std::move(moved.entry));
      }

      if (publishReset)
      {
        sortUniqueRowIndices(removeRowIndices);
        std::ranges::reverse(removeRowIndices);
      }
      else
      {
        appendRemovalRangesDescending(batch.deltas, removeRowIndices);
      }

      for (auto const rowIndex : removeRowIndices)
      {
        orderIndex.erase(orderIndex.begin() + static_cast<std::ptrdiff_t>(rowIndex));
      }

      std::ranges::sort(sortedNew, std::ref(comparator));

      auto merged = std::vector<OrderEntry>{};
      merged.reserve(orderIndex.size() + sortedNew.size());
      std::ranges::merge(orderIndex, sortedNew, std::back_inserter(merged), std::ref(comparator));

      orderIndex = std::move(merged);
      rebuildRowIndex();

      auto insertRowIndices = std::vector<std::size_t>{};
      insertRowIndices.reserve(sortedNew.size());

      for (auto const& entry : sortedNew)
      {
        if (auto const optRowIndex = findRowIndex(entry.trackId); optRowIndex)
        {
          insertRowIndices.push_back(*optRowIndex);
        }
      }

      if (!publishReset)
      {
        appendAscendingRanges(batch.deltas,
                              insertRowIndices,
                              [](TrackRowRange range)
                              { return TrackListProjectionDelta{ProjectionInsertRange{range}}; });
      }
    }

    void updateUngroupedEntries(std::span<TrackId const> ids)
    {
      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      auto processed = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
      processed.reserve(ids.size());

      auto updateRowIndices = std::vector<std::size_t>{};
      updateRowIndices.reserve(ids.size());

      auto movedEntries = std::vector<PendingMovedEntry>{};
      movedEntries.reserve(ids.size());

      for (auto const id : ids)
      {
        if (!processed.insert(id).second)
        {
          continue;
        }

        auto const optRowIndex = findRowIndex(id);

        if (!optRowIndex)
        {
          continue;
        }

        auto const optView = storageValueOrNullopt(reader.get(id, loadMode), "Failed to update projected tracks");

        if (!optView)
        {
          continue;
        }

        auto newKeys = SortKeys{};
        fillSortKeys(newKeys, *optView, dictionary, sortBy, normCache, stringArena, normScratch);

        auto& oldEntry = orderIndex[*optRowIndex];

        if (!comparator)
        {
          oldEntry.keys = newKeys;
          updateRowIndices.push_back(*optRowIndex);
          continue;
        }

        auto testEntry = OrderEntry{.trackId = id, .keys = newKeys};

        if (comparator(oldEntry, testEntry) == comparator(testEntry, oldEntry))
        {
          oldEntry.keys = newKeys;
          updateRowIndices.push_back(*optRowIndex);
          continue;
        }

        movedEntries.push_back(PendingMovedEntry{.oldRowIndex = *optRowIndex, .entry = std::move(testEntry)});
      }

      if (updateRowIndices.empty() && movedEntries.empty())
      {
        return;
      }

      auto const publishReset = !updateRowIndices.empty() && !movedEntries.empty();
      auto batch = TrackListProjectionDeltaBatch{.revision = ++rev};

      if (!publishReset)
      {
        appendAscendingRanges(batch.deltas,
                              updateRowIndices,
                              [](TrackRowRange range)
                              { return TrackListProjectionDelta{ProjectionUpdateRange{range}}; });
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

  LiveTrackListProjection::LiveTrackListProjection(ViewId viewId, TrackSource& source, library::MusicLibrary& library)
    : _implPtr{std::make_unique<Impl>(viewId, source, library)}
  {
    source.attach(this);
  }

  LiveTrackListProjection::~LiveTrackListProjection()
  {
    if (!_implPtr->sourceDestroyed)
    {
      _implPtr->source.detach(this);
    }
  }

  ViewId LiveTrackListProjection::viewId() const noexcept
  {
    return _implPtr->viewId;
  }

  std::uint64_t LiveTrackListProjection::revision() const noexcept
  {
    return _implPtr->rev;
  }

  void LiveTrackListProjection::setPresentation(TrackPresentationSpec const& presentation)
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
    if (_implPtr->groupBy == spec.groupBy && hasSameSortDirection(_implPtr->sortBy, spec.sortBy) &&
        _implPtr->comparator)
    {
      _implPtr->sortBy = std::move(spec.sortBy);
      _implPtr->comparator = buildComparator(_implPtr->sortBy);
      std::ranges::reverse(_implPtr->orderIndex);
      _implPtr->rebuildRowIndex();

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

  std::size_t LiveTrackListProjection::size() const noexcept
  {
    return _implPtr->orderIndex.size();
  }

  TrackId LiveTrackListProjection::trackIdAt(std::size_t index) const
  {
    if (index >= _implPtr->orderIndex.size())
    {
      return kInvalidTrackId;
    }

    return _implPtr->orderIndex[index].trackId;
  }

  std::optional<std::size_t> LiveTrackListProjection::indexOf(TrackId trackId) const noexcept
  {
    if (auto const it = _implPtr->rowIndexByTrackId.find(trackId); it != _implPtr->rowIndexByTrackId.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  TrackPresentationSpec LiveTrackListProjection::presentation() const
  {
    return TrackPresentationSpec{
      .id = _implPtr->id,
      .groupBy = _implPtr->groupBy,
      .sortBy = _implPtr->sortBy,
      .visibleFields = _implPtr->visibleFields,
      .redundantFields = _implPtr->redundantFields,
    };
  }

  std::size_t LiveTrackListProjection::groupCount() const noexcept
  {
    return _implPtr->sections.size();
  }

  TrackGroupSectionSnapshot LiveTrackListProjection::groupAt(std::size_t groupIndex) const
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

  std::optional<std::size_t> LiveTrackListProjection::groupIndexAt(std::size_t rowIndex) const
  {
    return _implPtr->findSectionIndexAt(rowIndex);
  }

  Subscription LiveTrackListProjection::subscribe(
    std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler)
  {
    handler(TrackListProjectionDeltaBatch{
      .revision = _implPtr->rev,
      .deltas = {ProjectionReset{}},
    });

    _implPtr->subscribers.push_back(std::move(handler));
    std::size_t const index = _implPtr->subscribers.size() - 1;

    return Subscription{[this, index] { _implPtr->subscribers[index] = {}; }};
  }

  void LiveTrackListProjection::onReset()
  {
    _implPtr->rebuildOrderIndex();
    _implPtr->publishDelta(TrackListProjectionDeltaBatch{
      .revision = ++_implPtr->rev,
      .deltas = {ProjectionReset{}},
    });
  }

  void LiveTrackListProjection::onInserted(TrackId id, std::size_t /*sourceIndex*/)
  {
    _implPtr->insertEntry(id);
  }

  void LiveTrackListProjection::onUpdated(TrackId id, std::size_t /*sourceIndex*/)
  {
    _implPtr->updateEntry(id);
  }

  void LiveTrackListProjection::onRemoved(TrackId id, std::size_t /*sourceIndex*/)
  {
    _implPtr->removeEntry(id);
  }

  void LiveTrackListProjection::onBulkInserted(std::span<TrackId const> ids)
  {
    _implPtr->insertEntries(ids);
  }

  void LiveTrackListProjection::onBulkUpdated(std::span<TrackId const> ids)
  {
    _implPtr->updateEntries(ids);
  }

  void LiveTrackListProjection::onBulkRemoved(std::span<TrackId const> ids)
  {
    _implPtr->removeEntries(ids);
  }

  void LiveTrackListProjection::publishDelta(TrackListProjectionDeltaBatch const& batch)
  {
    _implPtr->publishDelta(batch);
  }

  void LiveTrackListProjection::onSourceDestroyed()
  {
    _implPtr->sourceDestroyed = true;
  }
} // namespace ao::rt
