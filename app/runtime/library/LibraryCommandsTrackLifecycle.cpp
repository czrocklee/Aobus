// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/CueBinding.h"
#include "runtime/library/LibraryCommandsInternal.h"
#include "runtime/library/LibraryWriteLane.h"
#include "runtime/library/MediaTrack.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/async/Task.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileTimeConversion.h>
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
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <stop_token>
#include <string>
#include <system_error>
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

      auto const pathText = utility::pathToUtf8(path);
      auto const outsideRoot = [&pathText]
      { return makeError(Error::Code::InvalidInput, std::format("track file is outside music root: {}", pathText)); };
      auto const notFound = [&pathText]
      { return makeError(Error::Code::NotFound, std::format("track file not found under music root: {}", pathText)); };
      auto const escapesRoot = [](std::filesystem::path const& rel) { return rel.empty() || *rel.begin() == ".."; };

      auto const candidate = path.is_absolute() ? path : root / path;
      auto resolveEc = std::error_code{};
      auto const fullPath = std::filesystem::weakly_canonical(candidate, resolveEc);
      auto const tooLongToResolve = resolveEc == std::errc::filename_too_long;

      if (resolveEc && !tooLongToResolve)
      {
        return notFound();
      }

      auto rel = std::filesystem::path{};

      if (tooLongToResolve)
      {
        // A path too long for the platform to resolve is still measured
        // against the URI limit in its lexical form. Without resolution only
        // spellings compare, so it is also measured against the configured
        // root, which may reach the resolved root through a symlink.
        auto const lexical = candidate.lexically_normal();
        rel = lexical.lexically_relative(root);

        if (escapesRoot(rel))
        {
          rel = lexical.lexically_relative(library.rootPath().lexically_normal());
        }
      }
      else
      {
        rel = fullPath.lexically_relative(root);
      }

      if (escapesRoot(rel))
      {
        return outsideRoot();
      }

      auto uriRes = library::LibraryUri::parse(utility::pathToGenericUtf8(rel));

      if (!uriRes)
      {
        if (uriRes.error().code == Error::Code::ValueTooLarge)
        {
          return makeError(
            Error::Code::ValueTooLarge,
            std::format("track file path is too long for the library: {}: {}", pathText, uriRes.error().message));
        }

        return makeError(
          Error::Code::InvalidInput,
          std::format("track file path does not form a library URI: {}: {}", pathText, uriRes.error().message));
      }

      // The URI fits, but its full path exceeds what the platform can open.
      if (tooLongToResolve)
      {
        return makeError(
          Error::Code::IoError,
          std::format("track file path is too long for the platform: {}: {}", pathText, resolveEc.message()));
      }

      auto resolvedPathRes = uriRes->resolveUnder(root);

      if (!resolvedPathRes)
      {
        return outsideRoot();
      }

      if (!std::filesystem::is_regular_file(*resolvedPathRes, ec) || ec)
      {
        return notFound();
      }

      return ImportTarget{.fullPath = std::move(*resolvedPathRes), .uri = std::string{uriRes->value()}};
    }

    struct PreparedTrackImport final
    {
      ImportTarget target;
      MediaTrack mediaTrack;
      CueBinding binding;
      std::uint64_t fileSize = 0;
      FileTimestamp mtime{};
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

      auto sheetsRes = readCueDirectory(library.rootPath(), targetRes->fullPath.parent_path());

      if (!sheetsRes)
      {
        return std::unexpected{sheetsRes.error()};
      }

      auto binding = bindCueSheet(
        utility::pathToGenericUtf8(utility::pathFromUtf8(targetRes->uri).filename()), *mediaTrackRes, *sheetsRes);

      if (binding.state == CueBindingState::Undetermined)
      {
        return makeError(Error::Code::InvalidInput, binding.errorMessage);
      }

      auto fileEc = std::error_code{};
      auto const fileSize = std::filesystem::file_size(targetRes->fullPath, fileEc);
      auto const timestampRes = library::lastWriteTimestamp(targetRes->fullPath);

      if (fileEc)
      {
        return makeError(Error::Code::IoError, "Failed to inspect track file: " + fileEc.message());
      }

      if (!timestampRes)
      {
        return std::unexpected{timestampRes.error()};
      }

      return PreparedTrackImport{.target = std::move(*targetRes),
                                 .mediaTrack = std::move(*mediaTrackRes),
                                 .binding = std::move(binding),
                                 .fileSize = fileSize,
                                 .mtime = *timestampRes};
    }

    Result<> revalidateTrackImport(library::MusicLibrary const& library,
                                   std::filesystem::path const& path,
                                   PreparedTrackImport const& prepared)
    {
      auto targetRes = importTargetForPath(library, path);

      if (!targetRes)
      {
        return std::unexpected{targetRes.error()};
      }

      if (targetRes->fullPath != prepared.target.fullPath || targetRes->uri != prepared.target.uri)
      {
        return makeError(Error::Code::Conflict, "Track file path changed after preparation; retry the import");
      }

      auto ec = std::error_code{};
      auto const fileSize = std::filesystem::file_size(targetRes->fullPath, ec);

      if (ec)
      {
        return makeError(Error::Code::IoError, "Failed to revalidate track file: " + ec.message());
      }

      auto const timestampRes = library::lastWriteTimestamp(targetRes->fullPath);

      if (!timestampRes)
      {
        return std::unexpected{timestampRes.error()};
      }

      if (fileSize != prepared.fileSize || *timestampRes != prepared.mtime)
      {
        return makeError(Error::Code::Conflict, "Track file changed after preparation; retry the import");
      }

      // Re-read both candidate sets, including absence and sheet byte digests,
      // on the same admitted physical path used for the file proof.
      auto sheetsRes = readCueDirectory(library.rootPath(), targetRes->fullPath.parent_path());

      if (!sheetsRes)
      {
        return std::unexpected{sheetsRes.error()};
      }

      if (cueEvidenceFor(utility::pathToGenericUtf8(utility::pathFromUtf8(targetRes->uri).filename()), *sheetsRes) !=
          prepared.binding.evidence)
      {
        return makeError(Error::Code::Conflict, "CUE sheets changed after preparation; retry the import");
      }

      return {};
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
      std::vector<TrackId> provisionalTrackIds;
      std::string uri{};
      std::string title{};
      std::string artist{};
    };

    CreateTrackReply committedCreateTrackReply(CreateTrackFacts facts)
    {
      return CreateTrackReply{
        .trackId = facts.provisionalTrackIds.front(),
        .trackIds = std::move(facts.provisionalTrackIds),
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
                                                           PreparedTrackImport& prepared)
    {
      auto const& target = prepared.target;
      auto& mediaTrack = prepared.mediaTrack;
      auto writer = transaction.tracks();

      if (auto const optExistingManifest = writer.manifest(target.uri); optExistingManifest)
      {
        return makeError(Error::Code::Conflict, std::format("track file is already imported: {}", target.uri));
      }

      auto& builder = mediaTrack.builder();
      builder.property().uri(target.uri);
      auto title = std::string{builder.metadata().title()};
      auto artist = std::string{builder.metadata().artist()};

      auto ids = std::vector<TrackId>{};
      auto manifest = library::FileManifestBuilder::makeEmpty();
      manifest.fileSize(prepared.fileSize).mtime(prepared.mtime);

      if (prepared.binding.state == CueBindingState::Bound)
      {
        auto builders = makeCueTracks(mediaTrack, prepared.binding);
        title = builders.front().metadata().title();
        artist = builders.front().metadata().artist();

        for (auto& segmentBuilder : builders)
        {
          segmentBuilder.property().uri(target.uri);
        }

        auto const optTotalFrames = mediaTrack.optTotalFrames();
        AO_INVARIANT(optTotalFrames, "Bound CUE import lost its exact carrier extent");
        auto createRes = writer.createCarrier(builders, manifest, *optTotalFrames);

        if (!createRes)
        {
          return detail::storageError("Failed to create carrier Tracks", createRes.error());
        }

        ids = std::move(*createRes);
      }
      else
      {
        auto createRes = writer.create(builder, manifest);

        if (!createRes)
        {
          return detail::storageError("Failed to create track data", createRes.error());
        }

        ids.push_back(*createRes);
      }

      return CreateTrackFacts{
        .provisionalTrackIds = std::move(ids),
        .uri = target.uri,
        .title = title,
        .artist = artist,
      };
    }
  } // namespace

  async::Task<Result<DeleteTrackReply>> LibraryCommands::Impl::deleteTrackAsync(LibraryWriteLane::Submission submission,
                                                                                TrackId const trackId)
  {
    return detail::executeChangedWorkAsync<DeleteTrackReply>(
      std::move(submission),
      "Delete track",
      [this, trackId](library::LibraryWrite& transaction)
      { return applyDeleteTrackInTransaction(library, transaction, trackId); });
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::Impl::previewDeleteTrackAsync(
    LibraryWriteLane::Submission submission,
    TrackId const trackId)
  {
    return detail::previewChangedWorkAsync<DeleteTrackReply>(
      std::move(submission),
      [this, trackId](library::LibraryWrite& transaction)
      { return applyDeleteTrackInTransaction(library, transaction, trackId); });
  }

  async::Task<Result<CreateTrackReply>> LibraryCommands::Impl::createTrackFromFileAsync(
    LibraryWriteLane::Submission submission,
    std::filesystem::path path) const
  {
    auto preparedRes = prepareTrackImport(library, path);

    if (!preparedRes)
    {
      co_return std::unexpected{preparedRes.error()};
    }

    auto& prepared = *preparedRes;

    auto mutationRes = co_await LibraryWriteLane::beginInteractiveMutationAsync(
      std::move(submission),
      {},
      [this, &path, &prepared](std::stop_token) { return revalidateTrackImport(library, path, prepared); });

    if (!mutationRes)
    {
      co_return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto executionRes = co_await mutation.executeAsync(
      [&prepared](library::LibraryWrite& transaction) -> Result<OperationOutcome<CreateTrackFacts>>
      {
        auto factsRes = applyCreateTrackInTransaction(transaction, prepared);

        if (!factsRes)
        {
          return std::unexpected{factsRes.error()};
        }

        auto facts = std::move(*factsRes);
        auto trackIds = facts.provisionalTrackIds;
        return Changed<CreateTrackFacts>{
          .value = std::move(facts),
          .changeSet = LibraryChangeSet{.tracksInserted = std::move(trackIds)},
        };
      },
      "Create track");

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    AO_INVARIANT(executionRes->optCommittedRevision, "Track creation did not commit its generated identity");
    co_return committedCreateTrackReply(std::move(executionRes->value));
  }

  async::Task<Result<PreviewCreateTrackReply>> LibraryCommands::Impl::previewCreateTrackFromFileAsync(
    LibraryWriteLane::Submission submission,
    std::filesystem::path path) const
  {
    auto preparedRes = prepareTrackImport(library, path);

    if (!preparedRes)
    {
      co_return std::unexpected{preparedRes.error()};
    }

    auto& prepared = *preparedRes;

    auto mutationRes = co_await LibraryWriteLane::beginInteractiveMutationAsync(
      std::move(submission),
      {},
      [this, &path, &prepared](std::stop_token) { return revalidateTrackImport(library, path, prepared); });

    if (!mutationRes)
    {
      co_return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto factsRes = mutation.apply([&prepared](library::LibraryWrite& transaction)
                                   { return applyCreateTrackInTransaction(transaction, prepared); });
    mutation.abort();

    if (!factsRes)
    {
      co_return std::unexpected{factsRes.error()};
    }

    co_return previewCreateTrackReply(std::move(*factsRes));
  }
} // namespace ao::rt
