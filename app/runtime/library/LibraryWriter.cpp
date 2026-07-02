// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/tag/TagFile.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct PatchResult final
    {
      bool changedHot = false;
      bool changedCold = false;
    };

    PatchResult applyMetadataPatch(library::TrackBuilder& builder, MetadataPatch const& patch)
    {
      auto& meta = builder.metadata();
      auto result = PatchResult{};

      if (patch.optTitle)
      {
        meta.title(*patch.optTitle);
        result.changedHot = true;
      }

      if (patch.optArtist)
      {
        meta.artist(*patch.optArtist);
        result.changedHot = true;
      }

      if (patch.optAlbum)
      {
        meta.album(*patch.optAlbum);
        result.changedHot = true;
      }

      if (patch.optAlbumArtist)
      {
        meta.albumArtist(*patch.optAlbumArtist);
        result.changedHot = true;
      }

      if (patch.optGenre)
      {
        meta.genre(*patch.optGenre);
        result.changedHot = true;
      }

      if (patch.optComposer)
      {
        meta.composer(*patch.optComposer);
        result.changedHot = true;
      }

      if (patch.optWork)
      {
        meta.work(*patch.optWork);
        result.changedCold = true;
      }

      if (patch.optMovement)
      {
        meta.movement(*patch.optMovement);
        result.changedCold = true;
      }

      if (patch.optMovementNumber)
      {
        meta.movementNumber(*patch.optMovementNumber);
        result.changedCold = true;
      }

      if (patch.optMovementTotal)
      {
        meta.movementTotal(*patch.optMovementTotal);
        result.changedCold = true;
      }

      if (patch.optYear)
      {
        meta.year(*patch.optYear);
        result.changedHot = true;
      }

      if (patch.optTrackNumber)
      {
        meta.trackNumber(*patch.optTrackNumber);
        result.changedCold = true;
      }

      if (patch.optTrackTotal)
      {
        meta.trackTotal(*patch.optTrackTotal);
        result.changedCold = true;
      }

      if (patch.optDiscNumber)
      {
        meta.discNumber(*patch.optDiscNumber);
        result.changedCold = true;
      }

      if (patch.optDiscTotal)
      {
        meta.discTotal(*patch.optDiscTotal);
        result.changedCold = true;
      }

      for (auto const& [key, optValue] : patch.customUpdates)
      {
        if (key.empty())
        {
          continue;
        }

        builder.customMetadata().remove(key);

        if (optValue)
        {
          builder.customMetadata().add(key, *optValue);
        }

        result.changedCold = true;
      }

      return result;
    }

    void throwStorageError(char const* action, Error const& error)
    {
      auto const message = std::format("{}: {}", action, error.message);
      throwException<Exception>(std::string_view{message}, error.location);
    }

    template<typename Transaction>
    void commitOrThrow(Transaction& txn, char const* action)
    {
      if (auto result = txn.commit(); !result)
      {
        throwStorageError(action, result.error());
      }
    }
  } // namespace

  struct LibraryWriter::Impl final
  {
    library::MusicLibrary& library;
    LibraryChanges& changes;
  };

  LibraryWriter::LibraryWriter(library::MusicLibrary& library, LibraryChanges& changes)
    : _implPtr{std::make_unique<Impl>(library, changes)}
  {
  }

  LibraryWriter::~LibraryWriter() = default;

  UpdateTrackMetadataReply LibraryWriter::updateMetadata(std::span<TrackId const> trackIds, MetadataPatch const& patch)
  {
    auto txn = _implPtr->library.writeTransaction();
    auto writer = _implPtr->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        continue;
      }

      auto builder = library::TrackBuilder::fromView(*optView, _implPtr->library.dictionary());
      auto const patchResult = applyMetadataPatch(builder, patch);

      if (!patchResult.changedHot && !patchResult.changedCold)
      {
        continue;
      }

      if (patchResult.changedHot)
      {
        auto hotDataResult = builder.serializeHot(txn, _implPtr->library.dictionary());

        if (!hotDataResult)
        {
          throwStorageError("Failed to serialize hot track data", hotDataResult.error());
        }

        if (auto result = writer.updateHot(trackId, *hotDataResult); !result)
        {
          throwStorageError("Failed to update hot track data", result.error());
        }
      }

      if (patchResult.changedCold)
      {
        auto coldDataResult = builder.serializeCold(txn, _implPtr->library.dictionary(), _implPtr->library.resources());

        if (!coldDataResult)
        {
          throwStorageError("Failed to serialize cold track data", coldDataResult.error());
        }

        if (auto result =
              writer.updateCold(trackId,
                                coldDataResult->size(),
                                [&](std::span<std::byte> buf) { std::ranges::copy(*coldDataResult, buf.begin()); });
            !result)
        {
          throwStorageError("Failed to update cold track data", result.error());
        }
      }

      mutated.push_back(trackId);
    }

    commitOrThrow(txn, "Failed to commit metadata update");

    if (!mutated.empty())
    {
      _implPtr->changes.notifyTracksMutated(mutated);
    }

    return UpdateTrackMetadataReply{.mutatedIds = std::move(mutated)};
  }

  EditTrackTagsReply LibraryWriter::editTags(std::span<TrackId const> trackIds,
                                             std::span<std::string const> tagsToAdd,
                                             std::span<std::string const> tagsToRemove)
  {
    auto txn = _implPtr->library.writeTransaction();
    auto writer = _implPtr->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

      if (!optView)
      {
        continue;
      }

      auto builder = library::TrackBuilder::fromView(*optView, _implPtr->library.dictionary());
      auto& tags = builder.tags();
      bool changed = false;

      for (auto const& tag : tagsToAdd)
      {
        if (!std::ranges::contains(tags.names(), tag))
        {
          tags.add(tag);
          changed = true;
        }
      }

      for (auto const& tag : tagsToRemove)
      {
        if (std::ranges::contains(tags.names(), tag))
        {
          tags.remove(tag);
          changed = true;
        }
      }

      if (!changed)
      {
        continue;
      }

      auto hotDataResult = builder.serializeHot(txn, _implPtr->library.dictionary());

      if (!hotDataResult)
      {
        throwStorageError("Failed to serialize hot track data", hotDataResult.error());
      }

      if (auto result = writer.updateHot(trackId, *hotDataResult); !result)
      {
        throwStorageError("Failed to update hot track data", result.error());
      }

      mutated.push_back(trackId);
    }

    commitOrThrow(txn, "Failed to commit tag update");

    if (!mutated.empty())
    {
      _implPtr->changes.notifyTracksMutated(mutated);
    }

    return EditTrackTagsReply{.mutatedIds = std::move(mutated)};
  }

  ListId LibraryWriter::createList(ListDraft const& draft)
  {
    auto txn = _implPtr->library.writeTransaction();

    auto builder =
      library::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == ListKind::Smart)
    {
      builder.filter(draft.expression);
    }
    else
    {
      for (auto const id : draft.trackIds)
      {
        builder.tracks().add(id);
      }
    }

    auto const payload = builder.serialize();

    auto createResult = _implPtr->library.lists().writer(txn).create(payload);

    if (!createResult)
    {
      throwStorageError("Failed to create list", createResult.error());
    }

    auto const [listId, view] = *createResult;

    commitOrThrow(txn, "Failed to commit list creation");

    _implPtr->changes.notifyListsMutated({listId}, {});

    return listId;
  }

  bool LibraryWriter::updateList(ListDraft const& draft)
  {
    auto txn = _implPtr->library.writeTransaction();
    auto listWriter = _implPtr->library.lists().writer(txn);

    if (!listWriter.get(draft.listId))
    {
      return false;
    }

    auto builder =
      library::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == ListKind::Smart)
    {
      builder.filter(draft.expression);
    }
    else
    {
      for (auto const id : draft.trackIds)
      {
        builder.tracks().add(id);
      }
    }

    auto const payload = builder.serialize();

    if (auto result = listWriter.update(draft.listId, payload); !result)
    {
      throwStorageError("Failed to update list", result.error());
    }

    commitOrThrow(txn, "Failed to commit list update");

    _implPtr->changes.notifyListsMutated({draft.listId}, {});
    return true;
  }

  bool LibraryWriter::deleteList(ListId listId)
  {
    auto txn = _implPtr->library.writeTransaction();

    if (!_implPtr->library.lists().writer(txn).remove(listId))
    {
      return false;
    }

    commitOrThrow(txn, "Failed to commit list delete");

    _implPtr->changes.notifyListsMutated({}, {listId});
    return true;
  }

  bool LibraryWriter::deleteTrack(TrackId trackId)
  {
    auto txn = _implPtr->library.writeTransaction();

    auto writer = _implPtr->library.tracks().writer(txn);

    if (!writer.remove(trackId))
    {
      return false;
    }

    commitOrThrow(txn, "Failed to commit track delete");
    _implPtr->changes.notifyTracksMutated({trackId});
    return true;
  }

  std::optional<TrackId> LibraryWriter::createTrackFromFile(std::filesystem::path const& path)
  {
    auto tagFileResult = tag::TagFile::open(path);

    if (!tagFileResult)
    {
      return std::nullopt;
    }

    auto trackResult = (*tagFileResult)->loadTrack();

    if (!trackResult)
    {
      return std::nullopt;
    }

    auto txn = _implPtr->library.writeTransaction();
    auto writer = _implPtr->library.tracks().writer(txn);
    auto builder = *trackResult;
    auto const uriStr = path.string();
    builder.property().uri(uriStr);

    auto preparedResult = builder.prepare(txn, _implPtr->library.dictionary(), _implPtr->library.resources());

    if (!preparedResult)
    {
      throwStorageError("Failed to prepare track data", preparedResult.error());
    }

    auto& [preparedHot, preparedCold] = *preparedResult;
    auto createResult = library::createPreparedTrackData(writer, preparedHot, preparedCold);

    if (!createResult)
    {
      throwStorageError("Failed to create track data", createResult.error());
    }

    auto const [id, trackView] = *createResult;

    auto fileEc = std::error_code{};
    auto const fileSize = std::filesystem::file_size(path, fileEc);

    if (fileEc)
    {
      return std::nullopt;
    }

    auto const lastWriteTime = std::filesystem::last_write_time(path, fileEc);

    if (fileEc)
    {
      return std::nullopt;
    }

    auto manifestWriter = _implPtr->library.manifest().writer(txn);
    auto manifestBuilder = library::FileManifestBuilder::createNew();
    manifestBuilder.trackId(id)
      .fileSize(static_cast<std::uint64_t>(fileSize))
      .mtime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count()));

    if (auto putResult = manifestWriter.put(path.string(), manifestBuilder.serialize()); !putResult)
    {
      throwStorageError("Failed to update file manifest", putResult.error());
    }

    commitOrThrow(txn, "Failed to commit track creation");

    _implPtr->changes.notifyTracksMutated({id});
    return id;
  }
} // namespace ao::rt
