// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/TrackCommandService.h>
#include <ao/tag/TagFile.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace ao::rt
{
  TrackCommandService::TrackCommandService(library::MusicLibrary& library, LibraryMutationService& mutation)
    : _library{library}, _mutation{mutation}
  {
  }

  bool TrackCommandService::addTag(TrackId trackId, std::string tagName)
  {
    auto txn = _library.writeTransaction();
    auto writer = _library.tracks().writer(txn);
    auto const optTrackView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

    if (!optTrackView)
    {
      return false;
    }

    auto builder = library::TrackBuilder::fromView(*optTrackView, _library.dictionary());

    if (std::ranges::contains(builder.tags().names(), tagName))
    {
      return false;
    }

    builder.tags().add(std::move(tagName));

    auto const hotData = builder.serializeHot(txn, _library.dictionary());
    writer.updateHot(trackId, hotData);
    txn.commit();

    _mutation.notifyTracksMutated({trackId});
    return true;
  }

  bool TrackCommandService::removeTag(TrackId trackId, std::string tagName)
  {
    auto txn = _library.writeTransaction();
    auto writer = _library.tracks().writer(txn);
    auto const optTrackView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

    if (!optTrackView)
    {
      return false;
    }

    auto builder = library::TrackBuilder::fromView(*optTrackView, _library.dictionary());

    if (!std::ranges::contains(builder.tags().names(), tagName))
    {
      return false;
    }

    builder.tags().remove(tagName);

    auto const hotData = builder.serializeHot(txn, _library.dictionary());
    writer.updateHot(trackId, hotData);
    txn.commit();

    _mutation.notifyTracksMutated({trackId});
    return true;
  }

  bool TrackCommandService::deleteTrack(TrackId trackId)
  {
    auto txn = _library.writeTransaction();

    if (auto writer = _library.tracks().writer(txn); writer.remove(trackId))
    {
      txn.commit();
      _mutation.notifyTracksMutated({trackId});
      return true;
    }

    return false;
  }

  TrackId TrackCommandService::createTrackFromFile(std::filesystem::path const& path)
  {
    auto const optTagFile = tag::TagFile::open(path);

    if (!optTagFile)
    {
      return kInvalidTrackId;
    }

    auto txn = _library.writeTransaction();
    auto writer = _library.tracks().writer(txn);
    auto builder = optTagFile->loadTrack();
    auto const uriStr = path.string();
    builder.property().uri(uriStr);

    auto const [preparedHot, preparedCold] = builder.prepare(txn, _library.dictionary(), _library.resources());
    auto const [id, trackView] =
      writer.createHotCold(preparedHot.size(),
                           preparedCold.size(),
                           [&preparedHot, &preparedCold](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                           {
                             preparedHot.writeTo(hot);
                             preparedCold.writeTo(cold);
                           });

    auto manifestWriter = _library.manifest().writer(txn);
    auto manifestBuilder = library::FileManifestBuilder::createNew();
    manifestBuilder.trackId(id)
      .fileSize(static_cast<std::uint64_t>(std::filesystem::file_size(path)))
      .mtime(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::filesystem::last_write_time(path).time_since_epoch())
          .count());
    manifestWriter.put(path.string(), manifestBuilder.serialize());

    txn.commit();

    _mutation.notifyTracksMutated({id});
    return id;
  }
} // namespace ao::rt
