// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibrarySnapshot.h>

#include "runtime/TrackFieldReaderInternal.h"
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackRow.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    std::string resolveDictionaryId(library::DictionaryStore const& dictionary, DictionaryId id)
    {
      if (id == kInvalidDictionaryId)
      {
        return {};
      }

      return std::string{dictionary.getOrDefault(id)};
    }

    std::string joinResolvedTags(library::TrackView::TagProxy tags, library::DictionaryStore const& dictionary)
    {
      auto result = std::string{};

      for (auto const tagId : tags)
      {
        auto const tag = dictionary.getOrDefault(tagId);

        if (tag.empty())
        {
          continue;
        }

        if (!result.empty())
        {
          result.append(", ");
        }

        result.append(tag);
      }

      return result;
    }

    std::optional<std::filesystem::path> resolveLibraryPath(std::filesystem::path const& libraryRoot,
                                                            std::string_view uri)
    {
      if (uri.empty())
      {
        return std::nullopt;
      }

      auto parsedRes = library::LibraryUri::parse(uri);

      if (!parsedRes)
      {
        return std::nullopt;
      }

      auto resolvedRes = parsedRes->resolveUnder(libraryRoot);
      return resolvedRes ? std::optional<std::filesystem::path>{std::move(*resolvedRes)} : std::nullopt;
    }

    TrackRow rowDataFromView(TrackId id,
                             library::MusicLibrary const& library,
                             library::TrackView const& view,
                             library::ReadTransaction const& transaction)
    {
      auto const& dictionary = library.dictionary();
      auto const metadata = view.metadata();
      auto const property = view.property();
      auto const classical = view.classical();

      std::uint64_t fileSize = 0;
      std::uint64_t modifiedTime = 0;
      auto status = library::FileStatus::Available;

      if (auto const uri = property.uri(); !uri.empty())
      {
        auto const manifestReader = library.manifest().reader(transaction);

        if (auto optManifest = manifestReader.get(uri); optManifest)
        {
          fileSize = optManifest->fileSize();
          modifiedTime = optManifest->mtime();
          status = optManifest->status();
        }
      }

      return TrackRow{
        .id = id,
        .coverArtId = view.coverArt()
                        .primary()
                        .transform([](library::CoverArt cover) { return cover.resourceId; })
                        .value_or(kInvalidResourceId),
        .optUriPath = resolveLibraryPath(library.rootPath(), property.uri()),
        .title = std::string{metadata.title()},
        .artist = resolveDictionaryId(dictionary, metadata.artistId()),
        .album = resolveDictionaryId(dictionary, metadata.albumId()),
        .albumArtist = resolveDictionaryId(dictionary, metadata.albumArtistId()),
        .genre = resolveDictionaryId(dictionary, metadata.genreId()),
        .composer = resolveDictionaryId(dictionary, metadata.composerId()),
        .conductor = resolveDictionaryId(dictionary, classical.conductorId()),
        .ensemble = resolveDictionaryId(dictionary, classical.ensembleId()),
        .work = resolveDictionaryId(dictionary, classical.workId()),
        .movement = resolveDictionaryId(dictionary, classical.movementId()),
        .soloist = resolveDictionaryId(dictionary, classical.soloistId()),
        .tags = joinResolvedTags(view.tags(), dictionary),
        .duration = property.duration(),
        .year = metadata.year(),
        .discNumber = metadata.discNumber(),
        .discTotal = metadata.discTotal(),
        .trackNumber = metadata.trackNumber(),
        .trackTotal = metadata.trackTotal(),
        .movementNumber = classical.movementNumber(),
        .movementTotal = classical.movementTotal(),
        .sampleRate = property.sampleRate().raw(),
        .channels = property.channels().raw(),
        .bitDepth = property.bitDepth().raw(),
        .codec = property.codec(),
        .bitrate = property.bitrate().raw(),
        .fileSize = fileSize,
        .modifiedTime = modifiedTime,
        .status = status,
      };
    }

    ListNode listNodeDataFromView(ListId id, library::ListView const& view)
    {
      return ListNode{
        .id = id,
        .parentId = view.parentId(),
        .name = std::string{view.name()},
        .description = std::string{view.description()},
        .expression = std::string{view.filter()},
      };
    }
  } // namespace

  struct LibrarySnapshot::Impl final
  {
    library::MusicLibrary const& library;
    library::ReadTransaction transaction;
    std::uint64_t revision = 0;

    explicit Impl(library::MusicLibrary const& library)
      : library{library}, transaction{library.readTransaction()}, revision{library.libraryRevision(transaction)}
    {
    }

    /**
     * @brief Counts how many of @p trackIds carry each tag they collectively hold.
     *
     * A track that no longer exists is skipped rather than reported, so it
     * increments no counter: every count then falls below the selection size,
     * which is what collapses a shared-tag intersection to empty.
     */
    std::vector<std::pair<std::string, std::size_t>> aggregateSelectionTags(
      std::span<TrackId const> const trackIds) const
    {
      if (trackIds.empty())
      {
        return {};
      }

      auto const reader = library.tracks().reader(transaction);
      auto const& dictionary = library.dictionary();
      auto membershipCounts = std::unordered_map<DictionaryId, std::size_t>{};
      auto tagsOnTrack = std::vector<DictionaryId>{};

      for (auto const trackId : trackIds)
      {
        auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

        if (!optView || !optView->isHotValid())
        {
          continue;
        }

        tagsOnTrack.clear();

        for (auto const tagId : optView->tags())
        {
          if (!std::ranges::contains(tagsOnTrack, tagId))
          {
            tagsOnTrack.push_back(tagId);
            ++membershipCounts[tagId];
          }
        }
      }

      // Dictionary identities are unique. Resolve each name only once after
      // counting, and keep the byte-order presentation independent of hashing.
      auto counts = std::vector<std::pair<std::string, std::size_t>>{};
      counts.reserve(membershipCounts.size());

      for (auto const& [tagId, count] : membershipCounts)
      {
        if (auto const tag = dictionary.getOrDefault(tagId); !tag.empty())
        {
          counts.emplace_back(tag, count);
        }
      }

      std::ranges::sort(counts, {}, &std::pair<std::string, std::size_t>::first);
      return counts;
    }
  };

  LibrarySnapshot::LibrarySnapshot(library::MusicLibrary const& library)
    : _implPtr{std::make_unique<Impl>(library)}
  {
  }

  LibrarySnapshot::LibrarySnapshot(LibrarySnapshot&&) noexcept = default;
  LibrarySnapshot& LibrarySnapshot::operator=(LibrarySnapshot&&) noexcept = default;
  LibrarySnapshot::~LibrarySnapshot() = default;

  std::uint64_t LibrarySnapshot::revision() const noexcept
  {
    return _implPtr->revision;
  }

  std::optional<TrackRow> LibrarySnapshot::trackRow(TrackId id) const
  {
    auto const& library = _implPtr->library;
    auto const& transaction = _implPtr->transaction;
    auto const reader = library.tracks().reader(transaction);
    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

    if (!optView || !optView->isHotValid() || !optView->isColdValid())
    {
      return std::nullopt;
    }

    return rowDataFromView(id, library, *optView, transaction);
  }

  std::optional<std::string> LibrarySnapshot::formatTrack(TrackId const id, query::FormatPlan const& plan) const
  {
    auto const& library = _implPtr->library;
    auto const reader = library.tracks().reader(_implPtr->transaction);
    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

    if (!optView || !query::hasRequiredTrackData(plan.accessProfile, *optView))
    {
      return std::nullopt;
    }

    auto context = library::DictionaryReadContext{library.dictionary()};
    auto binding = query::FormatBinding{plan, context};
    auto output = std::string{};
    query::FormatEvaluator{}.evaluate(binding, *optView, output);
    return output;
  }

  bool LibrarySnapshot::containsTrack(TrackId const id) const
  {
    auto const reader = _implPtr->library.tracks().reader(_implPtr->transaction);
    return reader.get(id, library::TrackStore::Reader::LoadMode::Hot).has_value();
  }

  ResourceId LibrarySnapshot::trackCoverArtId(TrackId id) const
  {
    auto const reader = _implPtr->library.tracks().reader(_implPtr->transaction);
    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Cold);

    if (!optView || !optView->isColdValid())
    {
      return kInvalidResourceId;
    }

    return optView->coverArt()
      .primary()
      .transform([](library::CoverArt cover) { return cover.resourceId; })
      .value_or(kInvalidResourceId);
  }

  TrackFieldRawValue LibrarySnapshot::trackField(TrackId id, TrackField field) const
  {
    auto const& library = _implPtr->library;
    auto const& transaction = _implPtr->transaction;
    auto const reader = library.tracks().reader(transaction);
    auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

    if (!optView || !optView->isHotValid() || !optView->isColdValid())
    {
      return std::monostate{};
    }

    auto const manifestReader = library.manifest().reader(transaction);
    return readTrackFieldRawValue(field, *optView, library.dictionary(), &manifestReader);
  }

  std::string LibrarySnapshot::resolve(DictionaryId id) const
  {
    return resolveDictionaryId(_implPtr->library.dictionary(), id);
  }

  std::vector<ListNode> LibrarySnapshot::lists() const
  {
    auto const reader = _implPtr->library.lists().reader(_implPtr->transaction);
    auto result = std::vector<ListNode>{};

    for (auto const& [id, view] : reader)
    {
      result.push_back(listNodeDataFromView(id, view));
    }

    return result;
  }

  std::optional<ListNode> LibrarySnapshot::listNode(ListId id) const
  {
    auto const reader = _implPtr->library.lists().reader(_implPtr->transaction);
    auto const optView = reader.get(id);

    if (!optView)
    {
      return std::nullopt;
    }

    return listNodeDataFromView(id, *optView);
  }

  std::vector<TrackId> LibrarySnapshot::listOrderTrackIds(ListId id) const
  {
    auto const reader = _implPtr->library.lists().reader(_implPtr->transaction);
    auto const optView = reader.get(id);

    if (!optView)
    {
      return {};
    }

    auto ids = std::vector<TrackId>{};

    for (auto const trackId : optView->orderTrackIds())
    {
      ids.push_back(trackId);
    }

    return ids;
  }

  std::vector<std::string> LibrarySnapshot::selectionTags(std::span<TrackId const> const trackIds) const
  {
    if (trackIds.empty())
    {
      return {};
    }

    auto membershipCounts = _implPtr->aggregateSelectionTags(trackIds);
    auto const selectionCount = trackIds.size();
    auto shared = std::vector<std::string>{};

    // A tag shared by the whole selection is carried by exactly as many tracks
    // as the selection holds; anything less is carried by only some of them.
    for (auto& [tag, count] : membershipCounts)
    {
      if (count == selectionCount)
      {
        shared.push_back(std::move(tag));
      }
    }

    return shared;
  }

  std::vector<std::pair<std::string, std::size_t>> LibrarySnapshot::selectionTagCounts(
    std::span<TrackId const> const trackIds) const
  {
    return _implPtr->aggregateSelectionTags(trackIds);
  }

  std::vector<std::pair<std::string, std::size_t>> LibrarySnapshot::allTagsByFrequency() const
  {
    auto const& library = _implPtr->library;
    auto const reader = library.tracks().reader(_implPtr->transaction);
    auto const& dictionary = library.dictionary();

    auto frequencyByTag = std::map<std::string, std::size_t>{};

    for (auto const& [_, view] : reader.hot())
    {
      if (!view.isHotValid())
      {
        continue;
      }

      for (auto const tagId : view.tags())
      {
        if (auto tag = std::string{dictionary.getOrDefault(tagId)}; !tag.empty())
        {
          ++frequencyByTag[tag];
        }
      }
    }

    auto byFrequency = std::vector<std::pair<std::string, std::size_t>>{};
    byFrequency.reserve(frequencyByTag.size());

    for (auto const& [tag, frequency] : frequencyByTag)
    {
      byFrequency.emplace_back(tag, frequency);
    }

    std::ranges::sort(byFrequency,
                      [](auto const& lhs, auto const& rhs)
                      { return lhs.second > rhs.second || (lhs.second == rhs.second && lhs.first < rhs.first); });

    return byFrequency;
  }
} // namespace ao::rt
