// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/TrackFieldReaderInternal.h"
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/library/LibraryReader.h>

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

      auto const path = std::filesystem::path{uri};

      if (path.is_absolute())
      {
        return path.lexically_normal();
      }

      return (libraryRoot / path).lexically_normal();
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

        auto const optManifest = storageValueOrNullopt(manifestReader.get(uri), "Failed to load file manifest entry");

        if (optManifest)
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
        .kind = view.isSmart() ? ListNodeKind::Smart : ListNodeKind::Manual,
        .smartExpression = std::string{view.filter()},
      };
    }
  } // namespace

  struct LibraryReader::Impl final
  {
    library::MusicLibrary const& library;
    library::ReadTransaction transaction;

    explicit Impl(library::MusicLibrary const& library)
      : library{library}, transaction{library.readTransaction()}
    {
    }
  };

  LibraryReader::LibraryReader(library::MusicLibrary const& library)
    : _implPtr{std::make_unique<Impl>(library)}
  {
  }

  LibraryReader::LibraryReader(LibraryReader&&) noexcept = default;
  LibraryReader& LibraryReader::operator=(LibraryReader&&) noexcept = default;
  LibraryReader::~LibraryReader() = default;

  bool LibraryReader::isValid() const noexcept
  {
    return _implPtr != nullptr;
  }

  std::optional<TrackRow> LibraryReader::trackRow(TrackId id) const
  {
    auto const& library = _implPtr->library;
    auto const& transaction = _implPtr->transaction;
    auto const reader = library.tracks().reader(transaction);
    auto const optView =
      storageValueOrNullopt(reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to load track row");

    if (!optView)
    {
      return std::nullopt;
    }

    return rowDataFromView(id, library, *optView, transaction);
  }

  bool LibraryReader::containsTrack(TrackId const id) const
  {
    auto const reader = _implPtr->library.tracks().reader(_implPtr->transaction);
    return storageValueOrNullopt(
             reader.get(id, library::TrackStore::Reader::LoadMode::Hot), "Failed to check track existence")
      .has_value();
  }

  ResourceId LibraryReader::trackCoverArtId(TrackId id) const
  {
    auto const reader = _implPtr->library.tracks().reader(_implPtr->transaction);
    auto const optView = storageValueOrNullopt(
      reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to load track cover art");

    if (!optView)
    {
      return kInvalidResourceId;
    }

    return optView->coverArt()
      .primary()
      .transform([](library::CoverArt cover) { return cover.resourceId; })
      .value_or(kInvalidResourceId);
  }

  std::optional<std::filesystem::path> LibraryReader::trackUriPath(TrackId id) const
  {
    auto const& library = _implPtr->library;
    auto const reader = library.tracks().reader(_implPtr->transaction);
    auto const optView =
      storageValueOrNullopt(reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to load track URI");

    if (!optView)
    {
      return std::nullopt;
    }

    return resolveLibraryPath(library.rootPath(), optView->property().uri());
  }

  TrackFieldRawValue LibraryReader::trackField(TrackId id, TrackField field) const
  {
    auto const& library = _implPtr->library;
    auto const& transaction = _implPtr->transaction;
    auto const reader = library.tracks().reader(transaction);
    auto const optView =
      storageValueOrNullopt(reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to load track field");

    if (!optView)
    {
      return std::monostate{};
    }

    auto const manifestReader = library.manifest().reader(transaction);
    return readTrackFieldRawValue(field, *optView, library.dictionary(), &manifestReader);
  }

  std::string LibraryReader::resolve(DictionaryId id) const
  {
    return resolveDictionaryId(_implPtr->library.dictionary(), id);
  }

  std::vector<std::string> LibraryReader::resolveAll(std::span<DictionaryId const> ids) const
  {
    auto const& dictionary = _implPtr->library.dictionary();
    auto result = std::vector<std::string>{};
    result.reserve(ids.size());

    for (auto const id : ids)
    {
      result.push_back(resolveDictionaryId(dictionary, id));
    }

    return result;
  }

  std::vector<ListNode> LibraryReader::lists() const
  {
    auto const reader = _implPtr->library.lists().reader(_implPtr->transaction);
    auto result = std::vector<ListNode>{};

    for (auto const& [id, view] : reader)
    {
      result.push_back(listNodeDataFromView(id, view));
    }

    return result;
  }

  std::optional<ListNode> LibraryReader::listNode(ListId id) const
  {
    auto const reader = _implPtr->library.lists().reader(_implPtr->transaction);
    auto const optView = storageValueOrNullopt(reader.get(id), "Failed to load list node");

    if (!optView)
    {
      return std::nullopt;
    }

    return listNodeDataFromView(id, *optView);
  }

  std::vector<TrackId> LibraryReader::listTrackIds(ListId id) const
  {
    auto const reader = _implPtr->library.lists().reader(_implPtr->transaction);
    auto const optView = storageValueOrNullopt(reader.get(id), "Failed to load list tracks");

    if (!optView)
    {
      return {};
    }

    auto ids = std::vector<TrackId>{};

    for (auto const trackId : optView->tracks())
    {
      ids.push_back(trackId);
    }

    return ids;
  }

  std::optional<std::vector<std::byte>> LibraryReader::loadResource(ResourceId id) const
  {
    auto const reader = _implPtr->library.resources().reader(_implPtr->transaction);
    auto const optBytes = storageValueOrNullopt(reader.get(id), "Failed to load resource");

    if (!optBytes)
    {
      return std::nullopt;
    }

    return std::vector<std::byte>{optBytes->begin(), optBytes->end()};
  }

  std::vector<std::string> LibraryReader::selectionTags(std::span<TrackId const> trackIds) const
  {
    if (trackIds.empty())
    {
      return {};
    }

    auto const& library = _implPtr->library;
    auto const reader = library.tracks().reader(_implPtr->transaction);
    auto const& dictionary = library.dictionary();
    auto const selectionCount = trackIds.size();

    // Count how many selected tracks carry each tag; a tag shared by the whole
    // selection has a count equal to the selection size. Missing tracks never
    // increment any counter, so any stale id drives every count below the
    // threshold and the intersection collapses to empty.
    auto membershipCounts = std::map<std::string, std::size_t>{};

    for (auto const trackId : trackIds)
    {
      auto const optView = storageValueOrNullopt(
        reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot), "Failed to load track tags");

      if (!optView)
      {
        continue;
      }

      auto tagsOnTrack = std::vector<std::string>{};

      for (auto const tagId : optView->tags())
      {
        if (auto tag = std::string{dictionary.get(tagId)}; !tag.empty() && !std::ranges::contains(tagsOnTrack, tag))
        {
          tagsOnTrack.push_back(std::move(tag));
        }
      }

      for (auto const& tag : tagsOnTrack)
      {
        ++membershipCounts[tag];
      }
    }

    auto shared = std::vector<std::string>{};

    for (auto const& [tag, count] : membershipCounts)
    {
      if (count == selectionCount)
      {
        shared.push_back(tag);
      }
    }

    return shared;
  }

  std::vector<std::pair<std::string, std::size_t>> LibraryReader::allTagsByFrequency() const
  {
    auto const& library = _implPtr->library;
    auto const reader = library.tracks().reader(_implPtr->transaction);
    auto const& dictionary = library.dictionary();

    auto frequencyByTag = std::map<std::string, std::size_t>{};

    for (auto const& [_, view] : reader.hot())
    {
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
