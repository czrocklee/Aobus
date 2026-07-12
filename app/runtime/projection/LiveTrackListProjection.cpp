// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/PlaybackLaunchContext.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/Signal.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/projection/TrackProjectionEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceEditScript.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/utility/StringArena.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    constexpr std::size_t kYearStrLen = 5;
    constexpr std::size_t kMinimumArenaRebaseBytes = std::size_t{64} * 1024U;
    constexpr std::size_t kMinimumRowsBetweenRebases = 256;
    constexpr std::size_t kRebaseChurnDivisor = 4;

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

    bool isColdDataRequiredForSortField(TrackSortField field)
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

    bool isColdDataRequiredForGroupBy(TrackGroupKey groupBy)
    {
      return groupBy == TrackGroupKey::Work || groupBy == TrackGroupKey::Album || groupBy == TrackGroupKey::Conductor ||
             groupBy == TrackGroupKey::Ensemble;
    }

    library::TrackStore::Reader::LoadMode computeLoadMode(std::vector<TrackSortTerm> const& sortBy,
                                                          TrackGroupKey groupBy)
    {
      bool needsHot = groupBy != TrackGroupKey::None;
      bool needsCold = isColdDataRequiredForGroupBy(groupBy);

      for (auto const& term : sortBy)
      {
        if (isColdDataRequiredForSortField(term.field))
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
    TrackSourceLease sourceLease;
    library::MusicLibrary& library;
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy;
    std::string presentationId = std::string{kDefaultTrackPresentationId};
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

    std::vector<TrackId> sourceOrder;
    std::vector<OrderEntry> orderIndex;
    // Flat (open-addressing) map: contiguous bucket array, so rebuilding the index costs
    // no per-entry node allocation. Values are plain indices that nobody aliases, so the
    // rehash-on-grow relocation is safe here (unlike the arena-backed views, which stay
    // valid only because the arena bytes never move).
    boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>> rowIndexByTrackId;
    std::vector<GroupSection> sections;
    TrackListProjectionOperationCounts operationCounts;
    std::size_t rowsTouchedSinceRebuild = 0;
    std::size_t arenaRebaseThresholdBytes = kMinimumArenaRebaseBytes;
    std::uint64_t rev = 0;
    Signal<TrackListProjectionDeltaBatch const&> changedSignal;
    bool sourceInvalidated = false;
    Subscription sourceSubscription;

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

    Impl(ViewId vid,
         TrackSourceLease trackSourceLease,
         library::MusicLibrary& lib,
         std::vector<TrackSortTerm> initialSort = {})
      : viewId{vid}
      , sourceLease{std::move(trackSourceLease)}
      , library{lib}
      , sortBy{std::move(initialSort)}
      , comparator{buildComparator(sortBy)}
      , loadMode{computeLoadMode(sortBy, groupBy)}
    {
      if (sourceLease->state() == TrackSourceState::Live)
      {
        rebuildOrderIndex();
      }
    }

    void publishDelta(TrackListProjectionDeltaBatch const& batch)
    {
      if (!sourceInvalidated)
      {
        changedSignal.emit(batch);
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
      ++operationCounts.fullProjectionRebuilds;
      sourceOrder.clear();
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

      auto& source = sourceLease.source();
      sourceOrder.reserve(source.size());
      orderIndex.reserve(source.size());

      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto& dictionary = library.dictionary();

      for (std::size_t index = 0; index < source.size(); ++index)
      {
        auto const trackId = source.trackIdAt(index);
        sourceOrder.push_back(trackId);

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
      rowsTouchedSinceRebuild = 0;
      auto const allocatedBytes = stringArena.allocatedBytes();
      arenaRebaseThresholdBytes = allocatedBytes > std::numeric_limits<std::size_t>::max() / 2U
                                    ? std::numeric_limits<std::size_t>::max()
                                    : std::max(kMinimumArenaRebaseBytes, allocatedBytes * 2U);
    }

    void rebuildRowIndex()
    {
      ++operationCounts.rowIndexRebuilds;
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

    using TrackIdSet = boost::unordered_flat_set<TrackId, std::hash<TrackId>>;

    bool sourceMatches(std::span<TrackId const> expected) const
    {
      auto const& source = sourceLease.source();

      if (source.size() != expected.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < expected.size(); ++index)
      {
        if (source.trackIdAt(index) != expected[index])
        {
          return false;
        }
      }

      return true;
    }

    bool shouldRebase() const
    {
      auto const churnThreshold =
        std::max(kMinimumRowsBetweenRebases, (orderIndex.size() + kRebaseChurnDivisor - 1U) / kRebaseChurnDivisor);
      return rowsTouchedSinceRebuild >= churnThreshold || stringArena.allocatedBytes() >= arenaRebaseThresholdBytes;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one pass keeps batch invariants visible together.
    bool applyIncrementalBatch(TrackSourceDeltaBatch const& sourceBatch, delta::RegularTrackEditScript const& script)
    {
      bool const hasStructuralChanges = std::ranges::any_of(
        sourceBatch.deltas,
        [](TrackSourceDelta const& sourceDelta) { return !std::holds_alternative<SourceUpdateRange>(sourceDelta); });
      auto finalSourceOrderStorage = std::vector<TrackId>{};
      auto finalSourceOrder = std::span<TrackId const>{sourceOrder};

      if (hasStructuralChanges)
      {
        auto result = delta::apply(sourceOrder, script);

        if (!result)
        {
          return false;
        }

        finalSourceOrderStorage = std::move(*result);
        finalSourceOrder = finalSourceOrderStorage;
      }
      else
      {
        for (auto const& sourceDelta : sourceBatch.deltas)
        {
          auto const& update = std::get<SourceUpdateRange>(sourceDelta);

          if (update.start > sourceOrder.size() || update.trackIds.size() > sourceOrder.size() - update.start ||
              !std::ranges::equal(update.trackIds, finalSourceOrder.subspan(update.start, update.trackIds.size())))
          {
            return false;
          }
        }
      }

      if (!sourceMatches(finalSourceOrder))
      {
        return false;
      }

      auto replacementIds = TrackIdSet{};
      auto excludedIds = TrackIdSet{};
      auto changedIds = TrackIdSet{};
      bool const entriesDependOnTrackData = comparator || groupBy != TrackGroupKey::None;

      for (auto const& sourceDelta : sourceBatch.deltas)
      {
        std::visit(
          [&](auto const& range)
          {
            using Range = std::remove_cvref_t<decltype(range)>;

            if constexpr (std::same_as<Range, SourceInsertRange>)
            {
              replacementIds.insert(range.trackIds.begin(), range.trackIds.end());
              excludedIds.insert(range.trackIds.begin(), range.trackIds.end());
              changedIds.insert(range.trackIds.begin(), range.trackIds.end());
            }
            else if constexpr (std::same_as<Range, SourceRemoveRange>)
            {
              excludedIds.insert(range.trackIds.begin(), range.trackIds.end());
              changedIds.insert(range.trackIds.begin(), range.trackIds.end());
            }
            else if constexpr (std::same_as<Range, SourceUpdateRange>)
            {
              changedIds.insert(range.trackIds.begin(), range.trackIds.end());

              if (entriesDependOnTrackData)
              {
                replacementIds.insert(range.trackIds.begin(), range.trackIds.end());
                excludedIds.insert(range.trackIds.begin(), range.trackIds.end());
              }
            }
          },
          sourceDelta);
      }

      auto retainedEntries = std::vector<OrderEntry>{};
      retainedEntries.reserve(orderIndex.size());

      for (auto const& entry : orderIndex)
      {
        if (!excludedIds.contains(entry.trackId))
        {
          retainedEntries.push_back(entry);
        }
      }

      auto replacementEntries = std::vector<OrderEntry>{};
      replacementEntries.reserve(replacementIds.size());

      if (!replacementIds.empty())
      {
        auto const transaction = library.readTransaction();
        auto const reader = library.tracks().reader(transaction);
        auto& dictionary = library.dictionary();

        for (auto const trackId : finalSourceOrder)
        {
          if (!replacementIds.contains(trackId))
          {
            continue;
          }

          auto const optView =
            storageValueOrNullopt(reader.get(trackId, loadMode), "Failed to incrementally rebuild track order");

          if (!optView)
          {
            return false;
          }

          replacementEntries.push_back(entriesDependOnTrackData ? buildOrderEntry(trackId, *optView, dictionary)
                                                                : OrderEntry{.trackId = trackId});
        }
      }

      if (retainedEntries.size() + replacementEntries.size() != finalSourceOrder.size())
      {
        return false;
      }

      auto updatedOrder = std::vector<OrderEntry>{};
      updatedOrder.reserve(finalSourceOrder.size());

      if (comparator)
      {
        std::ranges::sort(replacementEntries, std::ref(comparator));
        std::ranges::merge(retainedEntries, replacementEntries, std::back_inserter(updatedOrder), std::ref(comparator));
      }
      else
      {
        auto retainedIndex = boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>>{};
        auto replacementIndex = boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>>{};
        retainedIndex.reserve(retainedEntries.size());
        replacementIndex.reserve(replacementEntries.size());

        for (std::size_t index = 0; index < retainedEntries.size(); ++index)
        {
          retainedIndex.emplace(retainedEntries[index].trackId, index);
        }

        for (std::size_t index = 0; index < replacementEntries.size(); ++index)
        {
          replacementIndex.emplace(replacementEntries[index].trackId, index);
        }

        for (auto const trackId : finalSourceOrder)
        {
          if (auto const replacementIt = replacementIndex.find(trackId); replacementIt != replacementIndex.end())
          {
            updatedOrder.push_back(replacementEntries[replacementIt->second]);
          }
          else if (auto const retainedIt = retainedIndex.find(trackId); retainedIt != retainedIndex.end())
          {
            updatedOrder.push_back(retainedEntries[retainedIt->second]);
          }
          else
          {
            return false;
          }
        }
      }

      if (hasStructuralChanges)
      {
        sourceOrder = std::move(finalSourceOrderStorage);
      }

      orderIndex = std::move(updatedOrder);
      ++operationCounts.incrementalProjectionUpdates;

      if (changedIds.size() > std::numeric_limits<std::size_t>::max() - rowsTouchedSinceRebuild)
      {
        rowsTouchedSinceRebuild = std::numeric_limits<std::size_t>::max();
      }
      else
      {
        rowsTouchedSinceRebuild += changedIds.size();
      }

      if (shouldRebase())
      {
        ++operationCounts.arenaRebases;
        rebuildOrderIndex();
      }
      else
      {
        rebuildRowIndex();
        buildGroupSections();
      }

      return true;
    }

    struct SectionDescriptor final
    {
      std::string groupKey;
      std::string primaryText;
      std::string secondaryText;
      std::string tertiaryText;
      ResourceId imageId{kInvalidResourceId};

      bool operator==(SectionDescriptor const&) const = default;
    };

    using TrackIndexMap = boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>>;

    std::vector<TrackId> projectionTrackIds() const
    {
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(orderIndex.size());

      for (auto const& entry : orderIndex)
      {
        trackIds.push_back(entry.trackId);
      }

      return trackIds;
    }

    std::vector<SectionDescriptor> sectionDescriptors() const
    {
      auto descriptors = std::vector<SectionDescriptor>{};
      descriptors.reserve(sections.size());

      for (auto const& section : sections)
      {
        descriptors.push_back(SectionDescriptor{
          .groupKey = std::string{section.groupKey},
          .primaryText = std::string{section.primaryText},
          .secondaryText = std::string{section.secondaryText},
          .tertiaryText = std::string{section.tertiaryText},
          .imageId = section.imageId,
        });
      }

      return descriptors;
    }

    static TrackIndexMap makeTrackIndex(std::span<TrackId const> trackIds)
    {
      auto indexByTrackId = TrackIndexMap{};
      indexByTrackId.reserve(trackIds.size());

      for (std::size_t index = 0; index < trackIds.size(); ++index)
      {
        indexByTrackId.emplace(trackIds[index], index);
      }

      return indexByTrackId;
    }

    static std::size_t finalSizeOf(TrackListProjectionDeltaBatch const& batch, std::size_t initialSize)
    {
      auto size = initialSize;

      for (auto const& delta : batch.deltas)
      {
        if (auto const* insertion = std::get_if<ProjectionInsertRange>(&delta); insertion != nullptr)
        {
          size += insertion->range.count;
        }
        else if (auto const* removal = std::get_if<ProjectionRemoveRange>(&delta); removal != nullptr)
        {
          size -= removal->range.count;
        }
      }

      return size;
    }

    void publishBatch(TrackListProjectionDeltaBatch batch, std::size_t previousSize)
    {
      if (sourceInvalidated)
      {
        return;
      }

      gsl_Assert(!batch.deltas.empty() && validateTrackListProjectionDeltaBatch(batch, previousSize) &&
                 !std::holds_alternative<ProjectionSourceInvalidated>(batch.deltas.front()));

      batch.revision = ++rev;
      changedSignal.emit(batch);
    }

    void publishReset(std::size_t previousSize)
    {
      publishBatch(TrackListProjectionDeltaBatch{.deltas = {ProjectionReset{}}}, previousSize);
    }

    void publishSourceInvalidated()
    {
      if (sourceInvalidated)
      {
        return;
      }

      sourceInvalidated = true;
      sourceSubscription.reset();
      auto const batch = TrackListProjectionDeltaBatch{
        .revision = ++rev,
        .deltas = {ProjectionSourceInvalidated{}},
      };
      changedSignal.emit(batch);
      changedSignal.disconnectAll();
    }

    static bool sourceOrderBatchMatches(std::vector<TrackId> const& previousTrackIds,
                                        TrackSourceDeltaBatch const& sourceBatch,
                                        std::vector<TrackId> const& finalTrackIds)
    {
      auto const script = regularTrackEditScriptOf(sourceBatch);

      if (!script)
      {
        return false;
      }

      auto const result = delta::apply(previousTrackIds, *script);
      return result && *result == finalTrackIds;
    }

    static TrackListProjectionDeltaBatch sourceOrderProjectionBatch(TrackSourceDeltaBatch const& sourceBatch)
    {
      auto const script = regularTrackEditScriptOf(sourceBatch);
      return script ? eraseTrackIds(*script) : TrackListProjectionDeltaBatch{};
    }

    void publishSortedSourceBatch(std::vector<TrackId> const& previousTrackIds,
                                  TrackSourceDeltaBatch const& sourceBatch,
                                  std::size_t previousSize)
    {
      auto const finalTrackIds = projectionTrackIds();
      auto updatedTrackIds = std::vector<TrackId>{};

      for (auto const& sourceDelta : sourceBatch.deltas)
      {
        if (auto const* update = std::get_if<SourceUpdateRange>(&sourceDelta); update != nullptr)
        {
          updatedTrackIds.append_range(update->trackIds);
        }
      }

      auto const previousIndex = makeTrackIndex(previousTrackIds);
      auto const finalIndex = makeTrackIndex(finalTrackIds);
      auto preferredMovedIds = std::vector<TrackId>{};

      for (auto const trackId : updatedTrackIds)
      {
        auto const previous = previousIndex.find(trackId);

        if (auto const final = finalIndex.find(trackId);
            previous != previousIndex.end() && final != finalIndex.end() && previous->second != final->second)
        {
          preferredMovedIds.push_back(trackId);
        }
      }

      auto const script = delta::diff(previousTrackIds, finalTrackIds, updatedTrackIds, preferredMovedIds);

      if (auto const applied = delta::apply(previousTrackIds, script); !applied || *applied != finalTrackIds)
      {
        publishReset(previousSize);
        return;
      }

      auto batch = eraseTrackIds(script);

      if (batch.deltas.empty())
      {
        return;
      }

      if (!validateTrackListProjectionDeltaBatch(batch, previousSize) ||
          finalSizeOf(batch, previousSize) != orderIndex.size())
      {
        publishReset(previousSize);
        return;
      }

      publishBatch(std::move(batch), previousSize);
    }

    void handleSourceBatch(TrackSourceDeltaBatch const& sourceBatch)
    {
      if (sourceInvalidated)
      {
        return;
      }

      if (sourceBatch.deltas.size() == 1 && std::holds_alternative<SourceInvalidated>(sourceBatch.deltas.front()))
      {
        publishSourceInvalidated();
        return;
      }

      auto const previousSize = orderIndex.size();
      auto const previousTrackIds = projectionTrackIds();
      auto const previousSections = sectionDescriptors();

      if (sourceBatch.deltas.size() == 1 && std::holds_alternative<SourceReset>(sourceBatch.deltas.front()))
      {
        rebuildOrderIndex();
        publishReset(previousSize);
        return;
      }

      if (auto const script = regularTrackEditScriptOf(sourceBatch);
          !script || !applyIncrementalBatch(sourceBatch, *script))
      {
        rebuildOrderIndex();
        publishReset(previousSize);
        return;
      }

      if (previousSections != sectionDescriptors())
      {
        publishReset(previousSize);
        return;
      }

      if (comparator)
      {
        publishSortedSourceBatch(previousTrackIds, sourceBatch, previousSize);
        return;
      }

      if (auto const finalTrackIds = projectionTrackIds();
          !sourceOrderBatchMatches(previousTrackIds, sourceBatch, finalTrackIds))
      {
        publishReset(previousSize);
        return;
      }

      auto batch = sourceOrderProjectionBatch(sourceBatch);

      if (!validateTrackListProjectionDeltaBatch(batch, previousSize) ||
          finalSizeOf(batch, previousSize) != orderIndex.size())
      {
        publishReset(previousSize);
        return;
      }

      publishBatch(std::move(batch), previousSize);
    }
  };

  LiveTrackListProjection::LiveTrackListProjection(ViewId viewId,
                                                   TrackSourceLease sourceLease,
                                                   library::MusicLibrary& library)
    : _implPtr{std::make_unique<Impl>(viewId, std::move(sourceLease), library)}
  {
    _implPtr->sourceSubscription = _implPtr->sourceLease->subscribe(
      [impl = _implPtr.get()](TrackSourceDeltaBatch const& batch) { impl->handleSourceBatch(batch); });
  }

  LiveTrackListProjection::LiveTrackListProjection(ViewId viewId,
                                                   TrackSourceLease sourceLease,
                                                   library::MusicLibrary& library,
                                                   TrackOrderSpec const& order)
    : _implPtr{std::make_unique<Impl>(viewId, std::move(sourceLease), library, order.sortBy)}
  {
    if (viewId != kInvalidViewId)
    {
      throwException<Exception>("Detached track-list projection requires an invalid view id");
    }

    _implPtr->sourceSubscription = _implPtr->sourceLease->subscribe(
      [impl = _implPtr.get()](TrackSourceDeltaBatch const& batch) { impl->handleSourceBatch(batch); });
  }

  LiveTrackListProjection::~LiveTrackListProjection() = default;

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
    if (_implPtr->sourceInvalidated)
    {
      return;
    }

    auto const previousSize = _implPtr->orderIndex.size();
    auto spec = normalizeTrackPresentationSpec(presentation);

    _implPtr->presentationId = spec.id;
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

      _implPtr->publishReset(previousSize);
      return;
    }

    _implPtr->groupBy = spec.groupBy;
    _implPtr->sortBy = std::move(spec.sortBy);
    _implPtr->comparator = buildComparator(_implPtr->sortBy);
    _implPtr->loadMode = computeLoadMode(_implPtr->sortBy, _implPtr->groupBy);

    _implPtr->rebuildOrderIndex();

    _implPtr->publishReset(previousSize);
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

  TrackListProjectionOperationCounts LiveTrackListProjection::operationCounts() const noexcept
  {
    return _implPtr->operationCounts;
  }

  TrackPresentationSpec LiveTrackListProjection::presentation() const
  {
    return TrackPresentationSpec{
      .id = _implPtr->presentationId,
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
    if (!handler)
    {
      throwException<Exception>("Track-list projection subscription handler must not be empty");
    }

    if (_implPtr->sourceInvalidated)
    {
      handler(TrackListProjectionDeltaBatch{
        .revision = _implPtr->rev,
        .deltas = {ProjectionSourceInvalidated{}},
      });
      return {};
    }

    auto handlerPtr =
      std::make_shared<std::move_only_function<void(TrackListProjectionDeltaBatch const&)>>(std::move(handler));
    auto subscription = _implPtr->changedSignal.connect([handlerPtr](TrackListProjectionDeltaBatch const& batch)
                                                        { (*handlerPtr)(batch); });

    (*handlerPtr)(TrackListProjectionDeltaBatch{
      .revision = _implPtr->rev,
      .deltas = {ProjectionReset{}},
    });

    return subscription;
  }
} // namespace ao::rt
