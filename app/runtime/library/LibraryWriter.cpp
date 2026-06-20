// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/tag/TagFile.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
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
  }

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
        auto const hotData = builder.serializeHot(txn, _implPtr->library.dictionary());
        writer.updateHot(trackId, hotData);
      }

      if (patchResult.changedCold)
      {
        auto const coldData = builder.serializeCold(txn, _implPtr->library.dictionary(), _implPtr->library.resources());
        writer.updateCold(trackId, coldData);
      }

      mutated.push_back(trackId);
    }

    txn.commit();

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

      auto const hotData = builder.serializeHot(txn, _implPtr->library.dictionary());
      writer.updateHot(trackId, hotData);
      mutated.push_back(trackId);
    }

    txn.commit();

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

    auto const [listId, view] = _implPtr->library.lists().writer(txn).create(payload);

    txn.commit();

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

    listWriter.update(draft.listId, payload);

    txn.commit();

    _implPtr->changes.notifyListsMutated({draft.listId}, {});
    return true;
  }

  bool LibraryWriter::deleteList(ListId listId)
  {
    auto txn = _implPtr->library.writeTransaction();

    if (!_implPtr->library.lists().writer(txn).del(listId))
    {
      return false;
    }

    txn.commit();

    _implPtr->changes.notifyListsMutated({}, {listId});
    return true;
  }

  bool LibraryWriter::deleteTrack(TrackId trackId)
  {
    auto txn = _implPtr->library.writeTransaction();

    if (auto writer = _implPtr->library.tracks().writer(txn); writer.remove(trackId))
    {
      txn.commit();
      _implPtr->changes.notifyTracksMutated({trackId});
      return true;
    }

    return false;
  }

  std::optional<TrackId> LibraryWriter::createTrackFromFile(std::filesystem::path const& path)
  {
    auto const tagFilePtr = tag::TagFile::open(path);

    if (!tagFilePtr)
    {
      return std::nullopt;
    }

    auto txn = _implPtr->library.writeTransaction();
    auto writer = _implPtr->library.tracks().writer(txn);
    auto builder = tagFilePtr->loadTrack();
    auto const uriStr = path.string();
    builder.property().uri(uriStr);

    auto const [preparedHot, preparedCold] =
      builder.prepare(txn, _implPtr->library.dictionary(), _implPtr->library.resources());
    auto const [id, trackView] =
      writer.createHotCold(preparedHot.size(),
                           preparedCold.size(),
                           [&preparedHot, &preparedCold](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                           {
                             preparedHot.writeTo(hot);
                             preparedCold.writeTo(cold);
                           });

    auto manifestWriter = _implPtr->library.manifest().writer(txn);
    auto manifestBuilder = library::FileManifestBuilder::createNew();
    manifestBuilder.trackId(id)
      .fileSize(static_cast<std::uint64_t>(std::filesystem::file_size(path)))
      .mtime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::filesystem::last_write_time(path).time_since_epoch())
          .count()));
    manifestWriter.put(path.string(), manifestBuilder.serialize());

    txn.commit();

    _implPtr->changes.notifyTracksMutated({id});
    return id;
  }
} // namespace ao::rt
