// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/projection/TrackListProjection.h>

#include "runtime/RuntimeOperationProbe.h"
#include "runtime/projection/StringArena.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/Enumerate.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/rt/projection/TrackProjectionEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <array>
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
#include <span>
#include <string>
#include <string_view>
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
    constexpr unsigned char kAsciiMax = 0x7fU;

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

    struct GroupIdentityKey final
    {
      std::string_view first{};
      std::string_view second{};

      bool operator==(GroupIdentityKey const&) const = default;
    };

    struct GroupSection final
    {
      TrackRowRange rows{};
      GroupIdentityKey identity{};
      using HeadingValue = std::variant<std::monostate, std::string_view, std::uint16_t, MissingTrackValueKind>;
      HeadingValue primary{};
      HeadingValue secondary{};
      HeadingValue tertiary{};
      ResourceId imageId{kInvalidResourceId};
    };

    TrackGroupHeadingValue ownHeadingValue(GroupSection::HeadingValue const& value)
    {
      return std::visit(
        []<typename Value>(Value const& item) -> TrackGroupHeadingValue
        {
          if constexpr (std::same_as<Value, std::string_view>)
          {
            return std::string{item};
          }
          else
          {
            return item;
          }
        },
        value);
    }

    struct OrderEntry final
    {
      TrackId trackId{};
      SortKeys keys{};
      GroupIdentityKey groupIdentity{};
      GroupSection::HeadingValue primary{};
      GroupSection::HeadingValue secondary{};
      GroupSection::HeadingValue tertiary{};
      ResourceId imageId{kInvalidResourceId};
    };

    using Comparator = compat::MoveOnlyFunction<bool(OrderEntry const&, OrderEntry const&)>;

    constexpr std::size_t kArticleAnLength = 3;

    bool startsWithCaseInsensitive(std::string_view str, std::string_view prefix)
    {
      if (str.size() < prefix.size())
      {
        return false;
      }

      for (std::size_t i = 0; i < prefix.size(); ++i)
      {
        if (utility::toAsciiLower(str[i]) != utility::toAsciiLower(prefix[i]))
        {
          return false;
        }
      }

      return true;
    }

    std::string_view stripLeadingArticle(std::string_view text)
    {
      if (startsWithCaseInsensitive(text, "the "))
      {
        text.remove_prefix(4);
      }
      else if (startsWithCaseInsensitive(text, "a "))
      {
        text.remove_prefix(2);
      }
      else if (startsWithCaseInsensitive(text, "an "))
      {
        text.remove_prefix(kArticleAnLength);
      }

      return text;
    }

    void foldAsciiInto(std::string& out, std::string_view text)
    {
      out.clear();
      out.reserve(text.size());

      for (auto const ch : text)
      {
        out.push_back(utility::toAsciiLower(ch));
      }
    }

    bool isAsciiText(std::string_view const text)
    {
      return std::ranges::all_of(text, [](char const ch) { return static_cast<unsigned char>(ch) <= kAsciiMax; });
    }

    void makeGroupIdentityKeyInto(std::string& out, std::string_view const text)
    {
      if (isAsciiText(text))
      {
        foldAsciiInto(out, text);
        return;
      }

      auto keyRes = utility::makeUtf8CaselessKey(text);
      AO_INVARIANT(
        keyRes.has_value(), "Admitted projection text failed Unicode identity folding: {}", keyRes.error().message);
      out = std::move(*keyRes);
    }

    // Article removal is ordering policy only. Group identity is built from the
    // unstripped text so values such as "The Doors" and "Doors" remain distinct.
    void makeOrderingKeyInto(std::string& out,
                             std::string_view const text,
                             TextOrderingPolicy const* const textOrderingPolicy)
    {
      auto const orderingText = stripLeadingArticle(text);

      if (textOrderingPolicy == nullptr)
      {
        makeGroupIdentityKeyInto(out, orderingText);
        return;
      }

      auto const keyRes = textOrderingPolicy->makeSortKeyInto(out, orderingText);
      AO_INVARIANT(
        keyRes.has_value(), "Admitted projection text failed locale sort-key derivation: {}", keyRes.error().message);
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

    bool hasRequiredTrackData(library::TrackView const& view, library::TrackStore::Reader::LoadMode loadMode)
    {
      switch (loadMode)
      {
        case library::TrackStore::Reader::LoadMode::Hot: return view.isHotValid();
        case library::TrackStore::Reader::LoadMode::Cold: return view.isColdValid();
        case library::TrackStore::Reader::LoadMode::Both: return view.isHotValid() && view.isColdValid();
      }

      return false;
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

    std::span<TrackSortField const> groupSortFields(TrackGroupKey const groupBy)
    {
      static constexpr auto kArtist = std::to_array({TrackSortField::Artist});
      static constexpr auto kAlbum = std::to_array({TrackSortField::AlbumArtist, TrackSortField::Album});
      static constexpr auto kAlbumArtist = std::to_array({TrackSortField::AlbumArtist});
      static constexpr auto kGenre = std::to_array({TrackSortField::Genre});
      static constexpr auto kComposer = std::to_array({TrackSortField::Composer});
      static constexpr auto kConductor = std::to_array({TrackSortField::Conductor});
      static constexpr auto kEnsemble = std::to_array({TrackSortField::Ensemble});
      static constexpr auto kWork = std::to_array({TrackSortField::Composer, TrackSortField::Work});
      static constexpr auto kYear = std::to_array({TrackSortField::Year});

      switch (groupBy)
      {
        case TrackGroupKey::Artist: return kArtist;
        case TrackGroupKey::Album: return kAlbum;
        case TrackGroupKey::AlbumArtist: return kAlbumArtist;
        case TrackGroupKey::Genre: return kGenre;
        case TrackGroupKey::Composer: return kComposer;
        case TrackGroupKey::Conductor: return kConductor;
        case TrackGroupKey::Ensemble: return kEnsemble;
        case TrackGroupKey::Work: return kWork;
        case TrackGroupKey::Year: return kYear;
        case TrackGroupKey::None: return {};
      }

      return {};
    }

    std::vector<TrackSortTerm> buildGroupOrder(TrackGroupKey const groupBy, std::vector<TrackSortTerm> const& sortBy)
    {
      auto groupOrder = std::vector<TrackSortTerm>{};
      groupOrder.reserve(2);
      auto const groupFields = groupSortFields(groupBy);

      for (auto const& term : sortBy)
      {
        if (std::ranges::contains(groupFields, term.field) &&
            !std::ranges::contains(groupOrder, term.field, &TrackSortTerm::field))
        {
          groupOrder.push_back(term);
        }
      }

      auto const fallbackAscending = groupOrder.empty() || groupOrder.front().ascending;
      auto const appendMissing = [&](TrackSortField const field)
      {
        if (!std::ranges::contains(groupOrder, field, &TrackSortTerm::field))
        {
          groupOrder.push_back(TrackSortTerm{.field = field, .ascending = fallbackAscending});
        }
      };

      for (auto const field : groupFields)
      {
        appendMissing(field);
      }

      return groupOrder;
    }

    std::string_view groupIdentityComponent(TrackGroupKey const groupBy,
                                            TrackSortField const field,
                                            GroupIdentityKey const& identity)
    {
      if (groupBy == TrackGroupKey::Album && field == TrackSortField::Album)
      {
        return identity.second;
      }

      if (groupBy == TrackGroupKey::Work && field == TrackSortField::Work)
      {
        return identity.second;
      }

      return identity.first;
    }

    std::int32_t compareGroupIdentity(TrackGroupKey const groupBy,
                                      std::span<TrackSortTerm const> const groupOrder,
                                      GroupIdentityKey const& lhs,
                                      GroupIdentityKey const& rhs)
    {
      for (auto const& term : groupOrder)
      {
        auto const lhsComponent = groupIdentityComponent(groupBy, term.field, lhs);
        auto const rhsComponent = groupIdentityComponent(groupBy, term.field, rhs);

        if (auto const cmp = lhsComponent.compare(rhsComponent); cmp != 0)
        {
          auto const normalized = cmp < 0 ? -1 : 1;
          return term.ascending ? normalized : -normalized;
        }
      }

      return 0;
    }

    Comparator buildComparator(std::vector<TrackSortTerm> sortBy, TrackGroupKey const groupBy)
    {
      auto groupOrder = buildGroupOrder(groupBy, sortBy);
      std::erase_if(sortBy,
                    [&groupOrder](TrackSortTerm const& term)
                    { return std::ranges::contains(groupOrder, term.field, &TrackSortTerm::field); });

      if (sortBy.empty() && groupOrder.empty())
      {
        return {};
      }

      return [sortBy = std::move(sortBy), groupOrder = std::move(groupOrder), groupBy](
               OrderEntry const& lhs, OrderEntry const& rhs) -> bool
      {
        for (auto const& term : groupOrder)
        {
          if (auto const cmp = compareSingleField(term, lhs.keys, rhs.keys); cmp != 0)
          {
            return term.ascending ? (cmp < 0) : (cmp > 0);
          }
        }

        if (auto const cmp = compareGroupIdentity(groupBy, groupOrder, lhs.groupIdentity, rhs.groupIdentity); cmp != 0)
        {
          return cmp < 0;
        }

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

    struct CachedDictionaryText final
    {
      std::string_view raw;
      std::string_view identityKey;
      std::string_view sortKey;
    };

    // Resolve a dictionary id once for display, group identity, and ordering.
    // Raw text borrows DictionaryStore's stable storage; derived keys are interned in the
    // projection arena. Empty nonzero slots are not cached because they can later be recycled.
    using DictionaryTextCache = boost::unordered_flat_map<DictionaryId, CachedDictionaryText, std::hash<DictionaryId>>;

    CachedDictionaryText dictionaryTextCached(DictionaryTextCache& textCache,
                                              detail::StringArena& arena,
                                              std::string& scratch,
                                              library::DictionaryStore const& dictionary,
                                              DictionaryId id,
                                              TextOrderingPolicy const* textOrderingPolicy)
    {
      if (auto const it = textCache.find(id); it != textCache.end())
      {
        return it->second;
      }

      auto const raw = dictionary.getOrDefault(id);
      makeGroupIdentityKeyInto(scratch, raw);
      auto const identityKey = arena.intern(scratch);
      auto const sortText = stripLeadingArticle(raw);
      auto sortKey = identityKey;

      if (textOrderingPolicy != nullptr || sortText.size() != raw.size())
      {
        makeOrderingKeyInto(scratch, raw, textOrderingPolicy);
        sortKey = arena.intern(scratch);
      }

      auto const text = CachedDictionaryText{.raw = raw, .identityKey = identityKey, .sortKey = sortKey};

      if (id == kInvalidDictionaryId || !raw.empty())
      {
        textCache.emplace(id, text);
      }

      return text;
    }

    void fillSortKey(SortKeys& keys,
                     library::TrackView const& view,
                     library::DictionaryStore const& dictionary,
                     TrackSortField const field,
                     DictionaryTextCache& textCache,
                     detail::StringArena& arena,
                     std::string& scratch,
                     TextOrderingPolicy const* textOrderingPolicy)
    {
      auto const sortText = [&](DictionaryId id) -> std::string_view
      { return dictionaryTextCached(textCache, arena, scratch, dictionary, id, textOrderingPolicy).sortKey; };

      switch (field)
      {
        case TrackSortField::Year: keys.year = view.metadata().year(); break;
        case TrackSortField::DiscNumber: keys.discNumber = view.metadata().discNumber(); break;
        case TrackSortField::TrackNumber: keys.trackNumber = view.metadata().trackNumber(); break;
        case TrackSortField::Movement: keys.movementNumber = view.classical().movementNumber(); break;
        case TrackSortField::Duration: keys.duration = view.property().duration(); break;
        case TrackSortField::Title:
          makeOrderingKeyInto(scratch, view.metadata().title(), textOrderingPolicy);
          keys.titleKey = arena.intern(scratch);
          break;
        case TrackSortField::Artist: keys.artistKey = sortText(view.metadata().artistId()); break;
        case TrackSortField::Album: keys.albumKey = sortText(view.metadata().albumId()); break;
        case TrackSortField::AlbumArtist: keys.albumArtistKey = sortText(view.metadata().albumArtistId()); break;
        case TrackSortField::Genre: keys.genreKey = sortText(view.metadata().genreId()); break;
        case TrackSortField::Composer: keys.composerKey = sortText(view.metadata().composerId()); break;
        case TrackSortField::Conductor: keys.conductorKey = sortText(view.classical().conductorId()); break;
        case TrackSortField::Ensemble: keys.ensembleKey = sortText(view.classical().ensembleId()); break;
        case TrackSortField::Work: keys.workKey = sortText(view.classical().workId()); break;
        case TrackSortField::Soloist: keys.soloistKey = sortText(view.classical().soloistId()); break;
      }
    }

    void fillSortKeys(SortKeys& keys,
                      library::TrackView const& view,
                      library::DictionaryStore const& dictionary,
                      std::span<TrackSortTerm const> const sortBy,
                      TrackGroupKey const groupBy,
                      DictionaryTextCache& textCache,
                      detail::StringArena& arena,
                      std::string& scratch,
                      TextOrderingPolicy const* textOrderingPolicy)
    {
      for (auto const& term : sortBy)
      {
        fillSortKey(keys, view, dictionary, term.field, textCache, arena, scratch, textOrderingPolicy);
      }

      for (auto const field : groupSortFields(groupBy))
      {
        if (!std::ranges::contains(sortBy, field, &TrackSortTerm::field))
        {
          fillSortKey(keys, view, dictionary, field, textCache, arena, scratch, textOrderingPolicy);
        }
      }
    }

    std::string_view internPaddedYearKey(detail::StringArena& arena, std::uint16_t year)
    {
      auto buf = std::array<char, kYearStrLen>{};
      auto const res = std::format_to_n(buf.data(), buf.size(), "{:05}", year);
      return arena.intern(std::string_view{buf.data(), static_cast<std::size_t>(res.out - buf.data())});
    }

    void fillGroupMetadata(OrderEntry& entry,
                           library::TrackView const& view,
                           library::DictionaryStore const& dictionary,
                           TrackGroupKey groupBy,
                           DictionaryTextCache& textCache,
                           detail::StringArena& arena,
                           std::string& scratch,
                           TextOrderingPolicy const* textOrderingPolicy)
    {
      auto const dictionaryText = [&](DictionaryId id)
      { return dictionaryTextCached(textCache, arena, scratch, dictionary, id, textOrderingPolicy); };

      switch (groupBy)
      {
        case TrackGroupKey::None: return;
        case TrackGroupKey::Artist:
        {
          auto const text = dictionaryText(view.metadata().artistId());
          entry.groupIdentity.first = text.identityKey;
          entry.primary = text.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Artist}
                                           : GroupSection::HeadingValue{text.raw};
        }
        break;
        case TrackGroupKey::Album:
          if (auto const optPrimary = view.coverArt().primary(); optPrimary)
          {
            entry.imageId = optPrimary->resourceId;
          }

          {
            auto const album = dictionaryText(view.metadata().albumId());
            auto const albumArtist = dictionaryText(view.metadata().albumArtistId());
            entry.groupIdentity = {.first = albumArtist.identityKey, .second = album.identityKey};

            if (album.raw.empty())
            {
              entry.primary = MissingTrackValueKind::Album;
            }
            else
            {
              entry.primary = album.raw;
            }

            if (albumArtist.raw.empty())
            {
              entry.secondary = MissingTrackValueKind::Artist;
            }
            else
            {
              entry.secondary = albumArtist.raw;
            }

            if (auto year = view.metadata().year(); year != 0)
            {
              entry.tertiary = year;
            }
            else
            {
              entry.tertiary = MissingTrackValueKind::Year;
            }
          }

          break;
        case TrackGroupKey::AlbumArtist:
        {
          auto const text = dictionaryText(view.metadata().albumArtistId());
          entry.groupIdentity.first = text.identityKey;
          entry.primary = text.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Artist}
                                           : GroupSection::HeadingValue{text.raw};
        }
        break;
        case TrackGroupKey::Genre:
        {
          auto const text = dictionaryText(view.metadata().genreId());
          entry.groupIdentity.first = text.identityKey;
          entry.primary = text.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Genre}
                                           : GroupSection::HeadingValue{text.raw};
        }
        break;
        case TrackGroupKey::Composer:
        {
          auto const text = dictionaryText(view.metadata().composerId());
          entry.groupIdentity.first = text.identityKey;
          entry.primary = text.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Composer}
                                           : GroupSection::HeadingValue{text.raw};
        }
        break;
        case TrackGroupKey::Conductor:
        {
          auto const text = dictionaryText(view.classical().conductorId());
          entry.groupIdentity.first = text.identityKey;
          entry.primary = text.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Conductor}
                                           : GroupSection::HeadingValue{text.raw};
        }
        break;
        case TrackGroupKey::Ensemble:
        {
          auto const text = dictionaryText(view.classical().ensembleId());
          entry.groupIdentity.first = text.identityKey;
          entry.primary = text.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Ensemble}
                                           : GroupSection::HeadingValue{text.raw};
        }
        break;
        case TrackGroupKey::Work:
        {
          auto const work = dictionaryText(view.classical().workId());
          auto const composer = dictionaryText(view.metadata().composerId());
          entry.groupIdentity = {.first = composer.identityKey, .second = work.identityKey};
          entry.primary = work.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Work}
                                           : GroupSection::HeadingValue{work.raw};
          entry.secondary = composer.raw.empty() ? GroupSection::HeadingValue{MissingTrackValueKind::Composer}
                                                 : GroupSection::HeadingValue{composer.raw};
        }
        break;
        case TrackGroupKey::Year:
        {
          std::uint16_t const year = entry.keys.year;
          entry.groupIdentity.first = internPaddedYearKey(arena, year);
          entry.primary =
            (year == 0) ? GroupSection::HeadingValue{MissingTrackValueKind::Year} : GroupSection::HeadingValue{year};
        }

        break;
        default: break;
      }
    }

    bool hasSameSortFields(std::vector<TrackSortTerm> const& old, std::vector<TrackSortTerm> const& updated)
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

  struct TrackListProjection::Impl final
  {
    ViewId viewId;
    TrackSourceLease sourceLease;
    library::MusicLibrary const& library;
    TextOrderingPolicy const* textOrderingPolicy = nullptr;
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy;
    std::string presentationId = std::string{kDefaultTrackPresentationId};
    std::vector<TrackField> visibleFields;
    std::vector<TrackField> redundantFields;
    Comparator comparator;
    library::TrackStore::Reader::LoadMode loadMode = library::TrackStore::Reader::LoadMode::Hot;
    // Derived sort/group key views in orderIndex/sections/dictionaryTextCache point into
    // the arena (bump-allocated and content-deduplicated), so it is declared first to outlive
    // them. Raw cache views borrow the library dictionary, which outlives this projection.
    // normScratch is a reused buffer that keeps the per-track normalization path allocation-free.
    detail::StringArena stringArena;
    std::string normScratch;
    DictionaryTextCache dictionaryTextCache;

    std::vector<TrackId> sourceOrder;
    std::vector<OrderEntry> orderIndex;
    // Flat (open-addressing) map: contiguous bucket array, so rebuilding the index costs
    // no per-entry node allocation. Values are plain indices that nobody aliases, so the
    // rehash-on-grow relocation is safe here (unlike the arena-backed views, which stay
    // valid only because the arena bytes never move).
    boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>> rowIndexByTrackId;
    std::vector<GroupSection> sections;
    detail::TrackListProjectionOperationCounts operationCounts;
    std::size_t rowsTouchedSinceRebuild = 0;
    std::size_t arenaRebaseThresholdBytes = kMinimumArenaRebaseBytes;
    async::Signal<TrackListProjectionDeltaBatch const&> changedSignal;
    bool sourceInvalidated = false;
    async::Subscription sourceSubscription;

    OrderEntry buildOrderEntry(TrackId id, library::TrackView const& view, library::DictionaryStore const& dictionary)
    {
      auto entry = OrderEntry{.trackId = id};
      fillSortKeys(entry.keys,
                   view,
                   dictionary,
                   sortBy,
                   groupBy,
                   dictionaryTextCache,
                   stringArena,
                   normScratch,
                   textOrderingPolicy);

      if (groupBy != TrackGroupKey::None)
      {
        fillGroupMetadata(
          entry, view, dictionary, groupBy, dictionaryTextCache, stringArena, normScratch, textOrderingPolicy);
      }

      return entry;
    }

    Impl(ViewId vid,
         TrackSourceLease trackSourceLease,
         library::MusicLibrary const& lib,
         std::vector<TrackSortTerm> initialSort,
         TextOrderingPolicy const* orderingPolicy)
      : viewId{vid}
      , sourceLease{std::move(trackSourceLease)}
      , library{lib}
      , textOrderingPolicy{orderingPolicy}
      , sortBy{std::move(initialSort)}
      , comparator{buildComparator(sortBy, groupBy)}
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
        .identity = orderIndex[0].groupIdentity,
        .primary = orderIndex[0].primary,
        .secondary = orderIndex[0].secondary,
        .tertiary = orderIndex[0].tertiary,
        .imageId = orderIndex[0].imageId,
      });

      for (std::size_t index = 1; index < orderIndex.size(); ++index)
      {
        if (orderIndex[index].groupIdentity != orderIndex[index - 1].groupIdentity)
        {
          sections.push_back(GroupSection{
            .rows = {.start = index, .count = 1},
            .identity = orderIndex[index].groupIdentity,
            .primary = orderIndex[index].primary,
            .secondary = orderIndex[index].secondary,
            .tertiary = orderIndex[index].tertiary,
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
      auto const timer = rt::ScopedTimer{"TrackListProjection::rebuildOrderIndex"};
      ++operationCounts.fullProjectionRebuilds;
      sourceOrder.clear();
      orderIndex.clear();
      rowIndexByTrackId.clear();
      sections.clear();

      // A full rebuild discards every container that holds an arena-backed view, so this is
      // the one safe point to reclaim the arena: clear the view holders first, then the text
      // cache (whose derived values are arena views too), then the arena itself. Without
      // this the arena would only grow across presentation switches / resets, trading the
      // allocation wins for unbounded memory. Incremental insert/update/remove must NOT clear:
      // they keep existing entries whose views still point into the arena.
      dictionaryTextCache.clear();
      stringArena.clear();

      auto& source = sourceLease.source();
      sourceOrder.reserve(source.size());
      orderIndex.reserve(source.size());

      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto const& dictionary = library.dictionary();

      for (std::size_t index = 0; index < source.size(); ++index)
      {
        sourceOrder.push_back(source.trackIdAt(index));
      }

      auto const entriesDependOnTrackData = comparator || groupBy != TrackGroupKey::None;
      reader.visitTracks(sourceOrder,
                         loadMode,
                         [&](TrackId trackId, library::TrackView const& view)
                         {
                           if (!entriesDependOnTrackData || hasRequiredTrackData(view, loadMode))
                           {
                             orderIndex.push_back(buildOrderEntry(trackId, view, dictionary));
                           }
                         });

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

      for (auto const& [index, entry] : compat::views::enumerate(orderIndex))
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

    struct SourceOrderResolution final
    {
      bool hasStructuralChanges = false;
      std::vector<TrackId> finalTrackIds{};

      std::span<TrackId const> trackIds(std::span<TrackId const> currentTrackIds) const
      {
        return hasStructuralChanges ? std::span<TrackId const>{finalTrackIds} : currentTrackIds;
      }
    };

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

    std::optional<SourceOrderResolution> resolveFinalSourceOrder(delta::RegularTrackEditScript const& script) const
    {
      auto resolution = SourceOrderResolution{
        .hasStructuralChanges = std::ranges::any_of(script.edits,
                                                    [](delta::RegularTrackEdit const& edit)
                                                    { return !std::holds_alternative<delta::UpdateRange>(edit); })};
      auto finalSourceOrder = std::span<TrackId const>{sourceOrder};

      if (resolution.hasStructuralChanges)
      {
        auto result = delta::apply(sourceOrder, script);

        if (!result)
        {
          return std::nullopt;
        }

        resolution.finalTrackIds = std::move(*result);
        finalSourceOrder = resolution.finalTrackIds;
      }
      else
      {
        for (auto const& edit : script.edits)
        {
          if (auto const& update = std::get<delta::UpdateRange>(edit);
              update.start > sourceOrder.size() || update.trackIds.size() > sourceOrder.size() - update.start ||
              !std::ranges::equal(update.trackIds, finalSourceOrder.subspan(update.start, update.trackIds.size())))
          {
            return std::nullopt;
          }
        }
      }

      if (!sourceMatches(finalSourceOrder))
      {
        return std::nullopt;
      }

      return resolution;
    }

    static void collectChangedTrackIds(delta::RegularTrackEditScript const& script,
                                       bool const entriesDependOnTrackData,
                                       TrackIdSet& replacementIds,
                                       TrackIdSet& excludedIds,
                                       TrackIdSet& changedIds)
    {
      for (auto const& edit : script.edits)
      {
        std::visit(
          [&](auto const& range)
          {
            using Range = std::remove_cvref_t<decltype(range)>;

            if constexpr (std::same_as<Range, delta::InsertRange>)
            {
              replacementIds.insert(range.trackIds.begin(), range.trackIds.end());
              excludedIds.insert(range.trackIds.begin(), range.trackIds.end());
              changedIds.insert(range.trackIds.begin(), range.trackIds.end());
            }
            else if constexpr (std::same_as<Range, delta::RemoveRange>)
            {
              excludedIds.insert(range.trackIds.begin(), range.trackIds.end());
              changedIds.insert(range.trackIds.begin(), range.trackIds.end());
            }
            else if constexpr (std::same_as<Range, delta::UpdateRange>)
            {
              changedIds.insert(range.trackIds.begin(), range.trackIds.end());

              if (entriesDependOnTrackData)
              {
                replacementIds.insert(range.trackIds.begin(), range.trackIds.end());
                excludedIds.insert(range.trackIds.begin(), range.trackIds.end());
              }
            }
          },
          edit);
      }
    }

    std::vector<OrderEntry> retainOrderEntries(TrackIdSet const& excludedIds) const
    {
      auto retainedEntries = std::vector<OrderEntry>{};
      retainedEntries.reserve(orderIndex.size());

      for (auto const& entry : orderIndex)
      {
        if (!excludedIds.contains(entry.trackId))
        {
          retainedEntries.push_back(entry);
        }
      }

      return retainedEntries;
    }

    std::optional<std::vector<OrderEntry>> buildReplacementEntries(TrackIdSet const& replacementIds,
                                                                   std::span<TrackId const> finalSourceOrder,
                                                                   bool const entriesDependOnTrackData)
    {
      auto replacementEntries = std::vector<OrderEntry>{};
      replacementEntries.reserve(replacementIds.size());

      if (replacementIds.empty())
      {
        return replacementEntries;
      }

      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      auto const& dictionary = library.dictionary();

      for (auto const trackId : finalSourceOrder)
      {
        if (!replacementIds.contains(trackId))
        {
          continue;
        }

        auto const optView = reader.get(trackId, loadMode);

        if (!optView || (entriesDependOnTrackData && !hasRequiredTrackData(*optView, loadMode)))
        {
          return std::nullopt;
        }

        replacementEntries.push_back(entriesDependOnTrackData ? buildOrderEntry(trackId, *optView, dictionary)
                                                              : OrderEntry{.trackId = trackId});
      }

      return replacementEntries;
    }

    std::optional<std::vector<OrderEntry>> mergeIncrementalOrder(std::span<TrackId const> finalSourceOrder,
                                                                 std::vector<OrderEntry> retainedEntries,
                                                                 std::vector<OrderEntry> replacementEntries)
    {
      auto updatedOrder = std::vector<OrderEntry>{};
      updatedOrder.reserve(finalSourceOrder.size());

      if (comparator)
      {
        std::ranges::sort(replacementEntries, std::ref(comparator));
        std::ranges::merge(retainedEntries, replacementEntries, std::back_inserter(updatedOrder), std::ref(comparator));
        return updatedOrder;
      }

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
          return std::nullopt;
        }
      }

      return updatedOrder;
    }

    void finishIncrementalBatch(bool const hasStructuralChanges,
                                std::vector<TrackId> finalSourceOrderStorage,
                                std::vector<OrderEntry> updatedOrder,
                                std::size_t const changedCount)
    {
      if (hasStructuralChanges)
      {
        sourceOrder = std::move(finalSourceOrderStorage);
      }

      orderIndex = std::move(updatedOrder);
      ++operationCounts.incrementalProjectionUpdates;

      if (changedCount > std::numeric_limits<std::size_t>::max() - rowsTouchedSinceRebuild)
      {
        rowsTouchedSinceRebuild = std::numeric_limits<std::size_t>::max();
      }
      else
      {
        rowsTouchedSinceRebuild += changedCount;
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
    }

    bool applyIncrementalBatch(delta::RegularTrackEditScript const& script)
    {
      auto optSourceOrderResolution = resolveFinalSourceOrder(script);

      if (!optSourceOrderResolution)
      {
        return false;
      }

      auto sourceOrderResolution = std::move(*optSourceOrderResolution);
      auto const finalSourceOrder = sourceOrderResolution.trackIds(sourceOrder);
      auto replacementIds = TrackIdSet{};
      auto excludedIds = TrackIdSet{};
      auto changedIds = TrackIdSet{};
      bool const entriesDependOnTrackData = comparator || groupBy != TrackGroupKey::None;
      collectChangedTrackIds(script, entriesDependOnTrackData, replacementIds, excludedIds, changedIds);
      auto retainedEntries = retainOrderEntries(excludedIds);
      auto optReplacementEntries = buildReplacementEntries(replacementIds, finalSourceOrder, entriesDependOnTrackData);

      if (!optReplacementEntries || retainedEntries.size() + optReplacementEntries->size() != finalSourceOrder.size())
      {
        return false;
      }

      auto optUpdatedOrder =
        mergeIncrementalOrder(finalSourceOrder, std::move(retainedEntries), std::move(*optReplacementEntries));

      if (!optUpdatedOrder)
      {
        return false;
      }

      finishIncrementalBatch(sourceOrderResolution.hasStructuralChanges,
                             std::move(sourceOrderResolution.finalTrackIds),
                             std::move(*optUpdatedOrder),
                             changedIds.size());
      return true;
    }

    struct SectionFingerprint final
    {
      std::size_t sectionCount = 0;
      std::size_t hash = 0;

      bool operator==(SectionFingerprint const&) const = default;
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

    static void combineHash(std::size_t& seed, std::size_t const value) noexcept
    {
      constexpr std::size_t kGoldenRatio = 0x9e3779b9U;
      constexpr std::size_t kLeftShift = 6;
      constexpr std::size_t kRightShift = 2;
      seed ^= value + kGoldenRatio + (seed << kLeftShift) + (seed >> kRightShift);
    }

    static void combineHeadingHash(std::size_t& seed, GroupSection::HeadingValue const& heading) noexcept
    {
      combineHash(seed, heading.index());
      std::visit(
        [&seed]<typename Value>(Value const& value)
        {
          if constexpr (std::same_as<Value, std::monostate>)
          {
            combineHash(seed, 0);
          }
          else if constexpr (std::same_as<Value, MissingTrackValueKind>)
          {
            combineHash(seed, static_cast<std::size_t>(value));
          }
          else
          {
            combineHash(seed, std::hash<Value>{}(value));
          }
        },
        heading);
    }

    SectionFingerprint sectionFingerprint() const noexcept
    {
      auto fingerprint = SectionFingerprint{.sectionCount = sections.size()};

      for (auto const& section : sections)
      {
        combineHash(fingerprint.hash, std::hash<std::string_view>{}(section.identity.first));
        combineHash(fingerprint.hash, std::hash<std::string_view>{}(section.identity.second));
        combineHeadingHash(fingerprint.hash, section.primary);
        combineHeadingHash(fingerprint.hash, section.secondary);
        combineHeadingHash(fingerprint.hash, section.tertiary);
        combineHash(fingerprint.hash, std::hash<ResourceId>{}(section.imageId));
      }

      return fingerprint;
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

      AO_INVARIANT(!batch.deltas.empty() && validateTrackListProjectionDeltaBatch(batch, previousSize) &&
                   !std::holds_alternative<ProjectionSourceInvalidated>(batch.deltas.front()));

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
      sourceOrder.clear();
      orderIndex.clear();
      rowIndexByTrackId.clear();
      sections.clear();
      dictionaryTextCache.clear();
      stringArena.clear();
      normScratch.clear();
      rowsTouchedSinceRebuild = 0;
      auto const batch = TrackListProjectionDeltaBatch{.deltas = {ProjectionSourceInvalidated{}}};
      changedSignal.emit(batch);
      changedSignal.disconnectAll();
    }

    static bool sourceOrderBatchMatches(std::vector<TrackId> const& previousTrackIds,
                                        delta::RegularTrackEditScript const& script,
                                        std::vector<TrackId> const& finalTrackIds)
    {
      auto const result = delta::apply(previousTrackIds, script);
      return result && *result == finalTrackIds;
    }

    static TrackListProjectionDeltaBatch sourceOrderProjectionBatch(delta::RegularTrackEditScript const& script)
    {
      return eraseTrackIds(script);
    }

    void publishSortedSourceBatch(std::vector<TrackId> const& previousTrackIds,
                                  delta::RegularTrackEditScript const& sourceScript,
                                  std::size_t previousSize)
    {
      auto const finalTrackIds = projectionTrackIds();
      auto updatedTrackIds = std::vector<TrackId>{};

      for (auto const& edit : sourceScript.edits)
      {
        if (auto const* update = std::get_if<delta::UpdateRange>(&edit); update != nullptr)
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

      if (auto const appliedRes = delta::apply(previousTrackIds, script); !appliedRes || *appliedRes != finalTrackIds)
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

    void handleSourceBatch(TrackSourceDelta const& sourceBatch)
    {
      if (sourceInvalidated)
      {
        return;
      }

      if (std::holds_alternative<SourceInvalidated>(sourceBatch))
      {
        publishSourceInvalidated();
        return;
      }

      auto const previousSize = orderIndex.size();
      auto const previousTrackIds = projectionTrackIds();
      auto const previousSections = sectionFingerprint();

      if (std::holds_alternative<SourceReset>(sourceBatch))
      {
        rebuildOrderIndex();
        publishReset(previousSize);
        return;
      }

      auto const& script = std::get<delta::RegularTrackEditScript>(sourceBatch);

      if (!applyIncrementalBatch(script))
      {
        rebuildOrderIndex();
        publishReset(previousSize);
        return;
      }

      if (previousSections != sectionFingerprint())
      {
        publishReset(previousSize);
        return;
      }

      if (comparator)
      {
        publishSortedSourceBatch(previousTrackIds, script, previousSize);
        return;
      }

      if (auto const finalTrackIds = projectionTrackIds();
          !sourceOrderBatchMatches(previousTrackIds, script, finalTrackIds))
      {
        publishReset(previousSize);
        return;
      }

      auto batch = sourceOrderProjectionBatch(script);

      if (!validateTrackListProjectionDeltaBatch(batch, previousSize) ||
          finalSizeOf(batch, previousSize) != orderIndex.size())
      {
        publishReset(previousSize);
        return;
      }

      publishBatch(std::move(batch), previousSize);
    }
  };

  TrackListProjection::TrackListProjection(ViewId viewId,
                                           TrackSourceLease sourceLease,
                                           library::MusicLibrary const& library,
                                           TextOrderingPolicy const* textOrderingPolicy)
    : _implPtr{std::make_unique<Impl>(viewId,
                                      std::move(sourceLease),
                                      library,
                                      std::vector<TrackSortTerm>{},
                                      textOrderingPolicy)}
  {
    _implPtr->sourceSubscription = _implPtr->sourceLease->subscribe(
      [impl = _implPtr.get()](TrackSourceDelta const& batch) { impl->handleSourceBatch(batch); });
  }

  TrackListProjection::TrackListProjection(ViewId viewId,
                                           TrackSourceLease sourceLease,
                                           library::MusicLibrary const& library,
                                           TrackOrderSpec const& order,
                                           TextOrderingPolicy const* textOrderingPolicy)
    : _implPtr{std::make_unique<Impl>(viewId, std::move(sourceLease), library, order.sortBy, textOrderingPolicy)}
  {
    AO_EXPECTS(viewId == kInvalidViewId, "Detached track-list projection requires an invalid view id");

    _implPtr->sourceSubscription = _implPtr->sourceLease->subscribe(
      [impl = _implPtr.get()](TrackSourceDelta const& batch) { impl->handleSourceBatch(batch); });
  }

  TrackListProjection::~TrackListProjection() = default;

  ViewId TrackListProjection::viewId() const noexcept
  {
    return _implPtr->viewId;
  }

  void TrackListProjection::setPresentation(TrackPresentationSpec const& presentation)
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

    // Reuse already-materialized keys when only directions changed (or the same
    // presentation is applied again); sorting them performs no library reads.
    if (_implPtr->groupBy == spec.groupBy && hasSameSortFields(_implPtr->sortBy, spec.sortBy) && _implPtr->comparator)
    {
      _implPtr->sortBy = std::move(spec.sortBy);
      _implPtr->comparator = buildComparator(_implPtr->sortBy, _implPtr->groupBy);
      std::ranges::sort(_implPtr->orderIndex, std::ref(_implPtr->comparator));
      _implPtr->rebuildRowIndex();
      _implPtr->buildGroupSections();

      _implPtr->publishReset(previousSize);
      return;
    }

    _implPtr->groupBy = spec.groupBy;
    _implPtr->sortBy = std::move(spec.sortBy);
    _implPtr->comparator = buildComparator(_implPtr->sortBy, _implPtr->groupBy);
    _implPtr->loadMode = computeLoadMode(_implPtr->sortBy, _implPtr->groupBy);

    _implPtr->rebuildOrderIndex();

    _implPtr->publishReset(previousSize);
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
    if (auto const it = _implPtr->rowIndexByTrackId.find(trackId); it != _implPtr->rowIndexByTrackId.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  TrackPresentationSpec TrackListProjection::presentation() const
  {
    return TrackPresentationSpec{
      .id = _implPtr->presentationId,
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
      .heading =
        TrackGroupHeading{
          .primary = ownHeadingValue(section.primary),
          .secondary = ownHeadingValue(section.secondary),
          .tertiary = ownHeadingValue(section.tertiary),
        },
      .imageId = section.imageId,
    };
  }

  std::optional<std::size_t> TrackListProjection::groupIndexAt(std::size_t rowIndex) const
  {
    return _implPtr->findSectionIndexAt(rowIndex);
  }

  std::optional<TrackRowRange> TrackListProjection::groupRangeAt(std::size_t rowIndex) const noexcept
  {
    auto const optSectionIndex = _implPtr->findSectionIndexAt(rowIndex);

    if (!optSectionIndex)
    {
      return std::nullopt;
    }

    return _implPtr->sections[*optSectionIndex].rows;
  }

  async::Subscription TrackListProjection::subscribe(
    compat::MoveOnlyFunction<void(TrackListProjectionDeltaBatch const&)> handler)
  {
    AO_EXPECTS(static_cast<bool>(handler), "Track-list projection subscription handler must not be empty");

    if (_implPtr->sourceInvalidated)
    {
      handler(TrackListProjectionDeltaBatch{.deltas = {ProjectionSourceInvalidated{}}});
      return {};
    }

    auto handlerPtr =
      std::make_shared<compat::MoveOnlyFunction<void(TrackListProjectionDeltaBatch const&)>>(std::move(handler));
    auto subscription = _implPtr->changedSignal.connect([handlerPtr](TrackListProjectionDeltaBatch const& batch)
                                                        { (*handlerPtr)(batch); });

    (*handlerPtr)(TrackListProjectionDeltaBatch{.deltas = {ProjectionReset{}}});

    return subscription;
  }

  namespace detail
  {
    TrackListProjectionOperationCounts RuntimeOperationProbe::counts(TrackListProjection const& projection) noexcept
    {
      return projection._implPtr->operationCounts;
    }
  } // namespace detail
} // namespace ao::rt
