// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryWriteLane.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::library
{
  class LibraryWrite;
} // namespace ao::library

namespace ao::rt
{
  namespace detail
  {
    Result<std::string> normalizeRuntimeText(std::string_view value, std::string_view context);

    Result<std::vector<std::string>> normalizeTags(std::span<std::string const> tags);

    Result<> validateDisjointTags(std::span<std::string const> normalizedAdd,
                                  std::span<std::string const> normalizedRemove);

    std::unexpected<Error> storageError(char const* action, Error const& error);

    std::vector<TrackId> orderTrackIdsFrom(library::ListView const& view);

    library::ListBuilder listWithOrder(library::ListView const& view, std::span<TrackId const> orderTrackIds);

    Result<library::ListView> requireList(library::ListWriter const& listWriter, ListId listId);

    delta::RegularTrackEditScript removalScriptFor(std::span<TrackId const> storedTrackIds,
                                                   std::unordered_set<TrackId> const& selectedTrackIds);

    Result<EditTrackTagsReply> applyTagPatchInTransaction(library::MusicLibrary& library,
                                                          library::LibraryWrite& transaction,
                                                          std::span<TrackId const> trackIds,
                                                          std::span<std::string const> tagsToAdd,
                                                          std::span<std::string const> tagsToRemove);

    template<typename Operation,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>>
    async::Task<OperationResult> applyInteractivePreviewAsync(LibraryWriteLane::Submission submission,
                                                              Operation operation)
    {
      auto mutationRes = co_await LibraryWriteLane::beginInteractiveMutationAsync(std::move(submission));

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      auto mutation = std::move(*mutationRes);
      auto res = mutation.apply(std::move(operation));
      mutation.abort();
      co_return res;
    }

    template<typename Operation,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>,
             typename Value = OperationResultTraits<OperationResult>::ValueType>
    async::Task<Result<MutationExecution<Value>>> executeInteractiveMutationAsync(
      LibraryWriteLane::Submission submission,
      std::string operationName,
      Operation operation)
    {
      auto mutationRes = co_await LibraryWriteLane::beginInteractiveMutationAsync(std::move(submission));

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      auto mutation = std::move(*mutationRes);
      auto executionRes = co_await mutation.executeAsync(std::move(operation), std::move(operationName));
      co_return std::move(executionRes);
    }

    template<typename Reply, typename Operation>
    async::Task<Result<TrackAuthoringResult<Reply>>> executeBoundTrackAuthoringAsync(
      LibraryWriteLane::Submission submission,
      BoundTrackTargets targets,
      std::string operationName,
      Operation operation)
    {
      auto start = co_await LibraryWriteLane::beginAuthoringMutationAsync(std::move(submission), targets);
      auto result = TrackAuthoringResult<Reply>{.status = start.status};

      if (!start.optMutation)
      {
        co_return result;
      }

      auto executionRes = co_await start.optMutation->executeAsync(
        [&targets, operation = std::move(operation)](
          library::LibraryWrite& transaction) mutable -> Result<OperationOutcome<Reply>>
        { return operation(transaction, targets.trackIds()); },
        std::move(operationName));

      if (!executionRes)
      {
        co_return std::unexpected{executionRes.error()};
      }

      result.reply = std::move(executionRes->value);

      if (!executionRes->optCommittedRevision)
      {
        result.status = AuthoringStatus::NoOp;
        co_return result;
      }

      result.status = AuthoringStatus::Applied;
      result.optNextTargets.emplace(
        LibraryWriteLane::advanceBoundTargets(targets, *executionRes->optCommittedRevision));
      co_return result;
    }

    template<typename Reply>
    struct ChangedWork final
    {
      Reply reply{};
      LibraryChangeSet changeSet{};
    };

    template<typename Reply, typename Operation>
    async::Task<Result<Reply>> executeChangedWorkAsync(LibraryWriteLane::Submission submission,
                                                       std::string operationName,
                                                       Operation operation)
    {
      auto executionRes = co_await executeInteractiveMutationAsync(
        std::move(submission),
        std::move(operationName),
        [operation =
           std::move(operation)](library::LibraryWrite& transaction) mutable -> Result<OperationOutcome<Reply>>
        {
          auto workRes = operation(transaction);

          if (!workRes)
          {
            return std::unexpected{workRes.error()};
          }

          auto work = std::move(*workRes);
          return Changed<Reply>{
            .value = std::move(work.reply),
            .changeSet = std::move(work.changeSet),
          };
        });

      if (!executionRes)
      {
        co_return std::unexpected{executionRes.error()};
      }

      co_return std::move(executionRes->value);
    }

    template<typename Reply, typename Operation>
    async::Task<Result<Reply>> previewChangedWorkAsync(LibraryWriteLane::Submission submission, Operation operation)
    {
      auto workRes = co_await applyInteractivePreviewAsync(std::move(submission), std::move(operation));

      if (!workRes)
      {
        co_return std::unexpected{workRes.error()};
      }

      co_return std::move(workRes->reply);
    }
  } // namespace detail

  struct LibraryCommands::Impl final
  {
    async::Task<Result<UpdateTrackMetadataReply>> previewUpdateMetadataAsync(LibraryWriteLane::Submission submission,
                                                                             std::vector<TrackId> trackIds,
                                                                             MetadataPatch patch);
    async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>>
    applyUpdateMetadataAsync(LibraryWriteLane::Submission submission, BoundTrackTargets targets, MetadataPatch patch);
    async::Task<Result<EditTrackTagsReply>> previewEditTagsAsync(LibraryWriteLane::Submission submission,
                                                                 std::vector<TrackId> trackIds,
                                                                 std::vector<std::string> tagsToAdd,
                                                                 std::vector<std::string> tagsToRemove);
    async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> applyEditTagsAsync(
      LibraryWriteLane::Submission submission,
      BoundTrackTargets targets,
      std::vector<std::string> tagsToAdd,
      std::vector<std::string> tagsToRemove);
    async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>> applyUpdatePropertiesAsync(
      LibraryWriteLane::Submission submission,
      BoundTrackTargets targets,
      TrackPropertiesPatch patch);
    async::Task<Result<AddTracksToListReply>> previewAddTracksToListAsync(LibraryWriteLane::Submission submission,
                                                                          ListId listId,
                                                                          std::vector<TrackId> trackIds);
    async::Task<Result<TrackAuthoringResult<AddTracksToListReply>>>
    applyAddTracksToListAsync(LibraryWriteLane::Submission submission, ListId listId, BoundTrackTargets targets);
    async::Task<Result<RemoveTracksFromListReply>> previewRemoveTracksFromListAsync(
      LibraryWriteLane::Submission submission,
      ListId listId,
      std::vector<TrackId> trackIds);
    async::Task<Result<TrackAuthoringResult<RemoveTracksFromListReply>>>
    applyRemoveTracksFromListAsync(LibraryWriteLane::Submission submission, ListId listId, BoundTrackTargets targets);
    async::Task<Result<ListId>> createListAsync(LibraryWriteLane::Submission submission, ListDraft draft);
    async::Task<Result<>> previewCreateListAsync(LibraryWriteLane::Submission submission, ListDraft draft);
    async::Task<Result<UpdateListReply>> updateListAsync(LibraryWriteLane::Submission submission, ListDraft draft);
    async::Task<Result<UpdateListReply>> previewUpdateListAsync(LibraryWriteLane::Submission submission,
                                                                ListDraft draft);
    async::Task<Result<AuthoringResult<MoveListOrderReply>>> applyMoveListOrderAsync(
      LibraryWriteLane::Submission submission,
      BoundListOrder order,
      std::vector<TrackId> selectedTrackIds,
      std::optional<TrackId> optBeforeTrackId);
    async::Task<Result<AuthoringResult<ResetListOrderReply>>> applyResetListOrderAsync(
      LibraryWriteLane::Submission submission,
      BoundListOrder order);
    async::Task<Result<AuthoringResult<ForgetHiddenListOrderReply>>> applyForgetHiddenListOrderAsync(
      LibraryWriteLane::Submission submission,
      BoundListOrder order);
    async::Task<Result<DeleteListReply>> deleteListAsync(LibraryWriteLane::Submission submission,
                                                         ListId listId,
                                                         DeleteListOptions options);
    async::Task<Result<DeleteListReply>> previewDeleteListAsync(LibraryWriteLane::Submission submission,
                                                                ListId listId,
                                                                DeleteListOptions options);
    async::Task<Result<DeleteListSubtreeReply>> deleteListAndDescendantsAsync(LibraryWriteLane::Submission submission,
                                                                              ListId listId,
                                                                              DeleteListOptions options);
    async::Task<Result<DeleteListSubtreeReply>> previewDeleteListAndDescendantsAsync(
      LibraryWriteLane::Submission submission,
      ListId listId,
      DeleteListOptions options);
    async::Task<Result<DeleteTrackReply>> deleteTrackAsync(LibraryWriteLane::Submission submission, TrackId trackId);
    async::Task<Result<DeleteTrackReply>> previewDeleteTrackAsync(LibraryWriteLane::Submission submission,
                                                                  TrackId trackId);
    async::Task<Result<CreateTrackReply>> createTrackFromFileAsync(LibraryWriteLane::Submission submission,
                                                                   std::filesystem::path path) const;
    async::Task<Result<PreviewCreateTrackReply>> previewCreateTrackFromFileAsync(
      LibraryWriteLane::Submission submission,
      std::filesystem::path path) const;

    library::MusicLibrary& library;
    LibraryWriteLane& writeLane;
    async::Runtime& asyncRuntime;
  };
} // namespace ao::rt
