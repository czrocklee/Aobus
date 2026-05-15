// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LibraryMutationService.h"

#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/ImportWorker.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/model/ListDraft.h>
#include <ao/utility/ThreadUtils.h>
#include <runtime/CorePrimitives.h>
#include <runtime/StateTypes.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstdint>

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
    library::MusicLibrary& library;
    std::jthread importThread;
    Signal<std::vector<TrackId> const&> tracksMutatedSignal;
    Signal<LibraryMutationService::ListsMutated const&> listsMutatedSignal;
    Signal<std::size_t> importCompletedSignal;
    Signal<LibraryMutationService::ImportProgressUpdated const&> importProgressSignal;
  };

  LibraryMutationService::LibraryMutationService(IControlExecutor& executor, library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(executor, library)}
  {
  }

  LibraryMutationService::~LibraryMutationService() = default;

  Subscription LibraryMutationService::onTracksMutated(
    std::move_only_function<void(std::vector<TrackId> const&)> handler)
  {
    return _impl->tracksMutatedSignal.connect(std::move(handler));
  }

  Subscription LibraryMutationService::onListsMutated(std::move_only_function<void(ListsMutated const&)> handler)
  {
    return _impl->listsMutatedSignal.connect(std::move(handler));
  }

  Subscription LibraryMutationService::onImportCompleted(std::move_only_function<void(std::size_t)> handler)
  {
    return _impl->importCompletedSignal.connect(std::move(handler));
  }

  Subscription LibraryMutationService::onImportProgress(
    std::move_only_function<void(ImportProgressUpdated const&)> handler)
  {
    return _impl->importProgressSignal.connect(std::move(handler));
  }

  Result<UpdateTrackMetadataReply> LibraryMutationService::updateMetadata(std::vector<TrackId> const& trackIds,
                                                                          MetadataPatch const& patch)
  {
    auto txn = _impl->library.writeTransaction();
    auto writer = _impl->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto const optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        continue;
      }

      auto builder = library::TrackBuilder::fromView(*optView, _impl->library.dictionary());

      if (auto const patchResult = applyMetadataPatch(builder, patch);
          patchResult.changedHot || patchResult.changedCold)
      {
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
      }

      mutated.push_back(trackId);
    }

    txn.commit();

    _impl->tracksMutatedSignal.emit(mutated);

    return UpdateTrackMetadataReply{.mutatedIds = std::move(mutated)};
  }

  Result<EditTrackTagsReply> LibraryMutationService::editTags(std::vector<TrackId> const& trackIds,
                                                              std::vector<std::string> const& tagsToAdd,
                                                              std::vector<std::string> const& tagsToRemove)
  {
    auto txn = _impl->library.writeTransaction();
    auto writer = _impl->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto const optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

      if (!optView)
      {
        continue;
      }

      auto builder = library::TrackBuilder::fromView(*optView, _impl->library.dictionary());

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

    _impl->tracksMutatedSignal.emit(mutated);

    return EditTrackTagsReply{.mutatedIds = std::move(mutated)};
  }

  ImportFilesReply LibraryMutationService::importFiles(std::vector<std::filesystem::path> const& paths)
  {
    struct ResultContainer final
    {
      library::ImportWorker::ImportResult result;
    };
    auto const container = std::make_shared<ResultContainer>();

    auto const totalFiles = paths.size();
    auto const worker = std::make_shared<library::ImportWorker>(
      _impl->library,
      paths,
      [this, totalFiles](std::filesystem::path const& filePath, std::int32_t index)
      {
        _impl->executor.dispatch(
          [this, filePath, index, totalFiles]
          {
            auto const fraction = totalFiles > 0 ? static_cast<double>(index) / static_cast<double>(totalFiles) : 0.0;
            auto const message = "Importing: " + filePath.filename().string();
            _impl->importProgressSignal.emit(LibraryMutationService::ImportProgressUpdated{
              .fraction = fraction,
              .message = message,
            });
          });
      },
      [this, container]
      {
        _impl->executor.dispatch(
          [this, container]
          {
            _impl->importCompletedSignal.emit(container->result.insertedIds.size());

            if (!container->result.insertedIds.empty())
            {
              _impl->tracksMutatedSignal.emit(container->result.insertedIds);
            }
          });
      });

    _impl->importThread = std::jthread(
      [worker, container]
      {
        setCurrentThreadName("FileImport");
        worker->run();
        container->result = worker->result();
      });

    return ImportFilesReply{};
  }

  ListId LibraryMutationService::createList(model::ListDraft const& draft)
  {
    auto txn = _impl->library.writeTransaction();

    auto builder =
      library::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == model::ListKind::Smart)
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

    auto const [listId, view] = _impl->library.lists().writer(txn).create(payload);

    txn.commit();

    auto const ev = LibraryMutationService::ListsMutated{.upserted = {listId}, .deleted = {}};
    _impl->listsMutatedSignal.emit(ev);

    return listId;
  }

  void LibraryMutationService::updateList(model::ListDraft const& draft)
  {
    auto txn = _impl->library.writeTransaction();

    auto builder =
      library::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == model::ListKind::Smart)
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

    _impl->library.lists().writer(txn).update(draft.listId, payload);

    txn.commit();

    auto const ev = LibraryMutationService::ListsMutated{.upserted = {draft.listId}, .deleted = {}};
    _impl->listsMutatedSignal.emit(ev);
  }

  void LibraryMutationService::deleteList(ListId listId)
  {
    auto txn = _impl->library.writeTransaction();
    _impl->library.lists().writer(txn).del(listId);
    txn.commit();

    auto const ev = LibraryMutationService::ListsMutated{.upserted = {}, .deleted = {listId}};
    _impl->listsMutatedSignal.emit(ev);
  }
}
