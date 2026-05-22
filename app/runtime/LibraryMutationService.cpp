// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryMutationService.h"

#include "ao/Error.h"
#include "ao/Exception.h"
#include "ao/Type.h"
#include "ao/library/LibraryScanner.h"
#include "ao/library/ListBuilder.h"
#include "ao/library/ListStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/ScanPlanExecutor.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/utility/ThreadUtils.h"
#include "async/Runtime.h"
#include "async/Task.h"
#include "runtime/CorePrimitives.h"
#include "runtime/LibraryYamlExporter.h"
#include "runtime/LibraryYamlImporter.h"
#include "runtime/StateTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
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

      if (patch.optTotalTracks)
      {
        meta.totalTracks(*patch.optTotalTracks);
        result.changedCold = true;
      }

      if (patch.optDiscNumber)
      {
        meta.discNumber(*patch.optDiscNumber);
        result.changedCold = true;
      }

      if (patch.optTotalDiscs)
      {
        meta.totalDiscs(*patch.optTotalDiscs);
        result.changedCold = true;
      }

      return result;
    }
  }

  struct LibraryMutationService::Impl final
  {
    async::Runtime& asyncRuntime;
    library::MusicLibrary& library;
    std::jthread activeWorkerThread;

    Signal<std::vector<TrackId> const&> tracksMutatedSignal;
    Signal<LibraryMutationService::ListsMutated const&> listsMutatedSignal;
    Signal<std::size_t> libraryTaskCompletedSignal;
    Signal<LibraryMutationService::LibraryTaskProgressUpdated const&> libraryTaskProgressSignal;

    Impl(async::Runtime& ar, library::MusicLibrary& lib)
      : asyncRuntime{ar}, library{lib}
    {
    }
  };

  LibraryMutationService::LibraryMutationService(async::Runtime& asyncRuntime, library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(asyncRuntime, library)}
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

  Subscription LibraryMutationService::onLibraryTaskCompleted(std::move_only_function<void(std::size_t)> handler)
  {
    return _impl->libraryTaskCompletedSignal.connect(std::move(handler));
  }

  Subscription LibraryMutationService::onLibraryTaskProgress(
    std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler)
  {
    return _impl->libraryTaskProgressSignal.connect(std::move(handler));
  }

  Result<UpdateTrackMetadataReply> LibraryMutationService::updateMetadata(std::span<TrackId const> trackIds,
                                                                          MetadataPatch const& patch)
  {
    auto txn = _impl->library.writeTransaction();
    auto writer = _impl->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

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

  Result<EditTrackTagsReply> LibraryMutationService::editTags(std::span<TrackId const> trackIds,
                                                              std::span<std::string const> tagsToAdd,
                                                              std::span<std::string const> tagsToRemove)
  {
    auto txn = _impl->library.writeTransaction();
    auto writer = _impl->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

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

  async::Task<void> LibraryMutationService::importLibraryAsync(std::filesystem::path path)
  {
    co_await _impl->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryImport");
    auto importer = ao::rt::LibraryYamlImporter{_impl->library};

    if (auto const result = importer.importFromYaml(path); !result)
    {
      throwException<Exception>("Library import failed: {}", result.error().message);
    }

    co_await _impl->asyncRuntime.resumeOnControl();
  }

  async::Task<void> LibraryMutationService::exportLibraryAsync(std::filesystem::path path, rt::ExportMode mode)
  {
    co_await _impl->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryExport");
    auto exporter = ao::rt::LibraryYamlExporter{_impl->library};

    if (auto const result = exporter.exportToYaml(path, mode); !result)
    {
      throwException<Exception>("Library export failed: {}", result.error().message);
    }

    co_await _impl->asyncRuntime.resumeOnControl();
  }

  async::Task<library::ScanPlan> LibraryMutationService::buildScanPlanAsync()
  {
    co_await _impl->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryScanner");

    auto scanner = library::LibraryScanner{_impl->library};
    auto plan = scanner.buildPlan(
      [this](std::filesystem::path const& path)
      {
        _impl->asyncRuntime.controlExecutor().dispatch(
          [this, path]
          {
            _impl->libraryTaskProgressSignal.emit(LibraryMutationService::LibraryTaskProgressUpdated{
              .fraction = 0.0, // Indeterminate during scan
              .message = "Scanning: " + path.filename().string(),
            });
          });
      });

    co_await _impl->asyncRuntime.resumeOnControl();
    co_return plan;
  }

  async::Task<void> LibraryMutationService::applyScanPlanAsync(library::ScanPlan plan)
  {
    co_await _impl->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("ApplyScanPlan");

    auto resultIds = std::vector<TrackId>{};
    auto const totalItems = plan.items.size();

    auto executor = ao::library::ScanPlanExecutor{
      _impl->library,
      std::move(plan),
      [this, totalItems](std::filesystem::path const& filePath, std::int32_t index)
      {
        _impl->asyncRuntime.controlExecutor().dispatch(
          [this, filePath, index, totalItems]
          {
            auto const fraction = totalItems > 0 ? static_cast<double>(index) / static_cast<double>(totalItems) : 0.0;
            auto const message = "Updating: " + filePath.filename().string();
            _impl->libraryTaskProgressSignal.emit(LibraryMutationService::LibraryTaskProgressUpdated{
              .fraction = fraction,
              .message = message,
            });
          });
      },
      [] {}};

    executor.run();
    resultIds = executor.result().processedIds;

    co_await _impl->asyncRuntime.resumeOnControl();

    _impl->libraryTaskCompletedSignal.emit(resultIds.size());

    if (!resultIds.empty())
    {
      _impl->tracksMutatedSignal.emit(resultIds);
    }
  }

  ListId LibraryMutationService::createList(ListDraft const& draft)
  {
    auto txn = _impl->library.writeTransaction();

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

    auto const [listId, view] = _impl->library.lists().writer(txn).create(payload);

    txn.commit();

    auto const ev = LibraryMutationService::ListsMutated{.upserted = {listId}, .deleted = {}};
    _impl->listsMutatedSignal.emit(ev);

    return listId;
  }

  void LibraryMutationService::updateList(ListDraft const& draft)
  {
    auto txn = _impl->library.writeTransaction();

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

  void LibraryMutationService::notifyTracksMutated(std::vector<TrackId> trackIds)
  {
    _impl->tracksMutatedSignal.emit(trackIds);
  }

  void LibraryMutationService::notifyListsMutated(std::vector<ListId> upserted, std::vector<ListId> deleted)
  {
    _impl->listsMutatedSignal.emit({.upserted = std::move(upserted), .deleted = std::move(deleted)});
  }
} // namespace ao::rt
