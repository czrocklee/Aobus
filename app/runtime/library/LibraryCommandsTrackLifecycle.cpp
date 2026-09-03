// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryCommandsInternal.h"
#include "runtime/library/LibraryWriteLane.h"
#include "runtime/library/MediaTrack.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/utility/Path.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct ImportTarget final
    {
      std::filesystem::path fullPath;
      std::string uri;
    };

    Result<ImportTarget> importTargetForPath(library::MusicLibrary const& library, std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      auto const root = std::filesystem::weakly_canonical(library.rootPath(), ec);

      if (ec)
      {
        return makeError(
          Error::Code::IoError,
          std::format("failed to resolve music root '{}': {}", utility::pathToUtf8(library.rootPath()), ec.message()));
      }

      bool sawOutsideRoot = false;

      auto const resolveInsideRoot =
        [&root, &sawOutsideRoot](std::filesystem::path const& candidate) -> std::optional<ImportTarget>
      {
        auto ec = std::error_code{};
        auto fullPath = std::filesystem::weakly_canonical(candidate, ec);

        if (ec)
        {
          return std::nullopt;
        }

        auto const rel = fullPath.lexically_relative(root);

        auto uriRes = library::LibraryUri::parse(utility::pathToGenericUtf8(rel));

        if (!uriRes)
        {
          sawOutsideRoot = true;
          return std::nullopt;
        }

        auto resolvedPathRes = uriRes->resolveUnder(root);

        if (!resolvedPathRes)
        {
          sawOutsideRoot = true;
          return std::nullopt;
        }

        if (!std::filesystem::is_regular_file(*resolvedPathRes, ec) || ec)
        {
          return std::nullopt;
        }

        return ImportTarget{.fullPath = std::move(*resolvedPathRes), .uri = std::string{uriRes->value()}};
      };

      auto optTarget = resolveInsideRoot(path.is_absolute() ? path : root / path);

      if (!optTarget)
      {
        if (sawOutsideRoot)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("track file is outside music root: {}", utility::pathToUtf8(path)));
        }

        return makeError(
          Error::Code::NotFound, std::format("track file not found under music root: {}", utility::pathToUtf8(path)));
      }

      return std::move(*optTarget);
    }

    struct PreparedTrackImport final
    {
      ImportTarget target;
      MediaTrack mediaTrack;
    };

    Result<PreparedTrackImport> prepareTrackImport(library::MusicLibrary const& library,
                                                   std::filesystem::path const& path)
    {
      auto targetRes = importTargetForPath(library, path);

      if (!targetRes)
      {
        return std::unexpected{targetRes.error()};
      }

      auto mediaTrackRes = readMediaTrack(targetRes->fullPath);

      if (!mediaTrackRes)
      {
        return std::unexpected{mediaTrackRes.error()};
      }

      return PreparedTrackImport{.target = std::move(*targetRes), .mediaTrack = std::move(*mediaTrackRes)};
    }

    struct ListOrderRemovalResult final
    {
      std::vector<ListId> changedListIds{};
      std::vector<ListOrderChange> orderChanges{};
    };

    struct PendingListOrderRemoval final
    {
      ListId listId = kInvalidListId;
      library::ListBuilder list;
      ListOrderChange orderChange{};
    };

    Result<ListOrderRemovalResult> removeTrackFromListOrders(library::MusicLibrary& library,
                                                             library::LibraryWrite& transaction,
                                                             TrackId trackId)
    {
      auto updates = std::vector<PendingListOrderRemoval>{};

      {
        auto listReader = library.lists().reader(transaction);

        for (auto const& [listId, view] : listReader)
        {
          if (!std::ranges::contains(view.orderTrackIds(), trackId))
          {
            continue;
          }

          auto const storedTrackIds = detail::orderTrackIdsFrom(view);
          auto const selectedTrackIds = std::unordered_set{trackId};
          auto builder = library::ListBuilder::fromView(view);
          builder.orderTrackIds().remove(trackId);

          updates.push_back(PendingListOrderRemoval{
            .listId = listId,
            .list = std::move(builder),
            .orderChange =
              ListOrderChange{
                .listId = listId,
                .operation = detail::removalScriptFor(storedTrackIds, selectedTrackIds),
              },
          });
        }
      }

      auto listWriter = transaction.lists();
      auto result = ListOrderRemovalResult{};
      result.changedListIds.reserve(updates.size());
      result.orderChanges.reserve(updates.size());

      for (auto& update : updates)
      {
        if (auto updateRes = listWriter.update(update.listId, update.list); !updateRes)
        {
          return detail::storageError("Failed to remove deleted track from List order", updateRes.error());
        }

        result.changedListIds.push_back(update.listId);
        result.orderChanges.push_back(std::move(update.orderChange));
      }

      return result;
    }

    Result<detail::ChangedWork<DeleteTrackReply>> applyDeleteTrackInTransaction(library::MusicLibrary& library,
                                                                                library::LibraryWrite& transaction,
                                                                                TrackId const trackId)
    {
      auto writer = transaction.tracks();
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
      }

      auto const uri = std::string{optView->property().uri()};
      auto const title = std::string{optView->metadata().title()};
      auto changedListsRes = removeTrackFromListOrders(library, transaction, trackId);

      if (!changedListsRes)
      {
        return std::unexpected{changedListsRes.error()};
      }

      auto changedLists = std::move(*changedListsRes);
      auto work = detail::ChangedWork<DeleteTrackReply>{
        .reply =
          DeleteTrackReply{
            .trackId = trackId,
            .uri = uri,
            .title = title,
            .removedFromListIds = changedLists.changedListIds,
          },
      };

      auto removeRes = writer.remove(trackId);

      if (!removeRes)
      {
        return std::unexpected{removeRes.error()};
      }

      if (!*removeRes)
      {
        return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
      }

      work.changeSet = LibraryChangeSet{.tracksDeleted = {trackId},
                                        .listsUpserted = work.reply.removedFromListIds,
                                        .listOrderChanges = std::move(changedLists.orderChanges)};
      return work;
    }

    struct CreateTrackFacts final
    {
      TrackId provisionalTrackId{};
      std::string uri{};
      std::string title{};
      std::string artist{};
    };

    CreateTrackReply committedCreateTrackReply(CreateTrackFacts facts)
    {
      return CreateTrackReply{
        .trackId = facts.provisionalTrackId,
        .uri = std::move(facts.uri),
        .title = std::move(facts.title),
        .artist = std::move(facts.artist),
      };
    }

    PreviewCreateTrackReply previewCreateTrackReply(CreateTrackFacts facts)
    {
      return PreviewCreateTrackReply{
        .uri = std::move(facts.uri), .title = std::move(facts.title), .artist = std::move(facts.artist)};
    }

    Result<CreateTrackFacts> applyCreateTrackInTransaction(library::LibraryWrite& transaction,
                                                           ImportTarget const& target,
                                                           MediaTrack& mediaTrack)
    {
      auto writer = transaction.tracks();

      if (auto const optExistingManifest = writer.manifest(target.uri); optExistingManifest)
      {
        return makeError(Error::Code::Conflict, std::format("track file is already imported: {}", target.uri));
      }

      auto& builder = mediaTrack.builder();
      builder.property().uri(target.uri);
      auto const title = std::string{builder.metadata().title()};
      auto const artist = std::string{builder.metadata().artist()};

      auto fileEc = std::error_code{};
      auto const fileSize = std::filesystem::file_size(target.fullPath, fileEc);

      if (fileEc)
      {
        return makeError(
          Error::Code::IoError,
          std::format("failed to inspect track file '{}': {}", utility::pathToUtf8(target.fullPath), fileEc.message()));
      }

      auto const lastWriteTime = std::filesystem::last_write_time(target.fullPath, fileEc);

      if (fileEc)
      {
        return makeError(
          Error::Code::IoError,
          std::format(
            "failed to read track file timestamp '{}': {}", utility::pathToUtf8(target.fullPath), fileEc.message()));
      }

      auto manifestBuilder = library::FileManifestBuilder::makeEmpty();
      manifestBuilder.fileSize(static_cast<std::uint64_t>(fileSize))
        .mtime(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count()));

      auto createRes = writer.create(builder, manifestBuilder);

      if (!createRes)
      {
        return detail::storageError("Failed to create track data", createRes.error());
      }

      return CreateTrackFacts{
        .provisionalTrackId = *createRes,
        .uri = target.uri,
        .title = title,
        .artist = artist,
      };
    }
  } // namespace

  async::Task<Result<DeleteTrackReply>> LibraryCommands::Impl::deleteTrack(LibraryWriteLane::Submission submission,
                                                                           TrackId const trackId)
  {
    return detail::executeChangedWorkAsync<DeleteTrackReply>(
      std::move(submission),
      "Delete track",
      [this, trackId](library::LibraryWrite& transaction)
      { return applyDeleteTrackInTransaction(library, transaction, trackId); });
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::Impl::previewDeleteTrack(
    LibraryWriteLane::Submission submission,
    TrackId const trackId)
  {
    return detail::previewChangedWorkAsync<DeleteTrackReply>(
      std::move(submission),
      [this, trackId](library::LibraryWrite& transaction)
      { return applyDeleteTrackInTransaction(library, transaction, trackId); });
  }

  async::Task<Result<CreateTrackReply>> LibraryCommands::Impl::createTrackFromFile(
    LibraryWriteLane::Submission submission,
    std::filesystem::path path) const
  {
    auto preparedRes = prepareTrackImport(library, path);

    if (!preparedRes)
    {
      co_return std::unexpected{preparedRes.error()};
    }

    auto& prepared = *preparedRes;

    auto executionRes = co_await detail::executeInteractiveMutationAsync(
      std::move(submission),
      "Create track",
      [&prepared](library::LibraryWrite& transaction) -> Result<OperationOutcome<CreateTrackFacts>>
      {
        auto factsRes = applyCreateTrackInTransaction(transaction, prepared.target, prepared.mediaTrack);

        if (!factsRes)
        {
          return std::unexpected{factsRes.error()};
        }

        auto facts = std::move(*factsRes);
        auto const trackId = facts.provisionalTrackId;
        return Changed<CreateTrackFacts>{
          .value = std::move(facts),
          .changeSet = LibraryChangeSet{.tracksInserted = {trackId}},
        };
      });

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    AO_INVARIANT(executionRes->optCommittedRevision, "Track creation did not commit its generated identity");
    co_return committedCreateTrackReply(std::move(executionRes->value));
  }

  async::Task<Result<PreviewCreateTrackReply>> LibraryCommands::Impl::previewCreateTrackFromFile(
    LibraryWriteLane::Submission submission,
    std::filesystem::path path) const
  {
    auto preparedRes = prepareTrackImport(library, path);

    if (!preparedRes)
    {
      co_return std::unexpected{preparedRes.error()};
    }

    auto& prepared = *preparedRes;

    auto factsRes = co_await detail::applyInteractivePreviewAsync(
      std::move(submission),
      [&prepared](library::LibraryWrite& transaction)
      { return applyCreateTrackInTransaction(transaction, prepared.target, prepared.mediaTrack); });

    if (!factsRes)
    {
      co_return std::unexpected{factsRes.error()};
    }

    co_return previewCreateTrackReply(std::move(*factsRes));
  }
} // namespace ao::rt
