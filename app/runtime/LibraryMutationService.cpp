// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LibraryMutationService.h"
#include "EventBus.h"
#include "EventTypes.h"

#include <ao/library/ImportWorker.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/utility/ThreadUtils.h>

#include <algorithm>
#include <memory>
#include <stop_token>
#include <vector>

namespace ao::app
{
  namespace
  {
    struct PatchResult final
    {
      bool changedHot = false;
      bool changedCold = false;
    };

    auto applyMetadataPatch(ao::library::TrackBuilder& builder, MetadataPatch const& patch) -> PatchResult
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

      return result;
    }
  }

  struct LibraryMutationService::Impl final
  {
    IControlExecutor& executor;
    EventBus& events;
    ao::library::MusicLibrary& library;
    std::jthread importThread;
  };

  LibraryMutationService::LibraryMutationService(EventBus& events,
                                                 IControlExecutor& executor,
                                                 ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(executor, events, library)}
  {
  }

  LibraryMutationService::~LibraryMutationService() = default;

  ao::Result<UpdateTrackMetadataReply> LibraryMutationService::updateMetadata(std::vector<ao::TrackId> const& trackIds,
                                                                              MetadataPatch const& patch)
  {
    auto txn = _impl->library.writeTransaction();
    auto writer = _impl->library.tracks().writer(txn);
    auto mutated = std::vector<ao::TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto const optView = writer.get(trackId, ao::library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        continue;
      }

      auto builder = ao::library::TrackBuilder::fromView(*optView, _impl->library.dictionary());
      auto patchResult = applyMetadataPatch(builder, patch);

      if (patchResult.changedHot)
      {
        auto const hotData = builder.serializeHot(txn, _impl->library.dictionary());
        writer.updateHot(trackId, hotData);
      }

      if (patchResult.changedCold)
      {
        auto const coldData = builder.serializeCold(txn, _impl->library.dictionary(), _impl->library.resources());
        writer.updateCold(trackId, coldData);
      }

      mutated.push_back(trackId);
    }

    txn.commit();

    _impl->events.publish(TracksMutated{.trackIds = mutated});

    return UpdateTrackMetadataReply{.mutatedIds = std::move(mutated)};
  }

  ao::Result<EditTrackTagsReply> LibraryMutationService::editTags(std::vector<ao::TrackId> const& trackIds,
                                                                  std::vector<std::string> const& tagsToAdd,
                                                                  std::vector<std::string> const& tagsToRemove)
  {
    auto txn = _impl->library.writeTransaction();
    auto writer = _impl->library.tracks().writer(txn);
    auto mutated = std::vector<ao::TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto const optView = writer.get(trackId, ao::library::TrackStore::Reader::LoadMode::Hot);
      if (!optView)
      {
        continue;
      }

      auto builder = ao::library::TrackBuilder::fromView(*optView, _impl->library.dictionary());

      auto& tags = builder.tags();
      for (auto const& tag : tagsToAdd)
      {
        tags.add(tag);
      }
      for (auto const& tag : tagsToRemove)
      {
        tags.remove(tag);
      }

      auto const hotData = builder.serializeHot(txn, _impl->library.dictionary());
      writer.updateHot(trackId, hotData);
      mutated.push_back(trackId);
    }

    txn.commit();

    _impl->events.publish(TracksMutated{.trackIds = mutated});

    return EditTrackTagsReply{.mutatedIds = std::move(mutated)};
  }

  ImportFilesReply LibraryMutationService::importFiles(std::vector<std::filesystem::path> const& paths)
  {
    auto totalFiles = paths.size();
    auto worker = std::make_shared<ao::library::ImportWorker>(
      _impl->library,
      paths,
      [this, totalFiles](std::filesystem::path const& filePath, std::int32_t index)
      {
        _impl->executor.dispatch(
          [this, filePath, index, totalFiles]()
          {
            _impl->events.publish(ImportProgressUpdated{
              .fraction = totalFiles > 0 ? static_cast<double>(index) / static_cast<double>(totalFiles) : 0.0,
              .message = "Importing: " + filePath.filename().string(),
            });
          });
      },
      []() {});

    _impl->importThread = std::jthread(
      [this, worker]()
      {
        ao::setCurrentThreadName("FileImport");
        worker->run();

        auto const& result = worker->result();
        _impl->executor.dispatch(
          [this, ids = result.insertedIds, count = result.insertedIds.size()]()
          {
            _impl->events.publish(TracksMutated{.trackIds = ids});
            _impl->events.publish(LibraryImportCompleted{.importedTrackCount = count});
          });
      });

    return ImportFilesReply{.importedTrackCount = 0};
  }
}
