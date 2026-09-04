// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryCommands.h>

#include "runtime/library/LibraryCommandsInternal.h"
#include "runtime/library/LibraryWriteLane.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/utility/UnicodeText.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    Result<std::vector<std::string>> normalizeTags(std::span<std::string const> const tags)
    {
      auto normalized = std::vector<std::string>{};
      normalized.reserve(tags.size());
      auto seen = std::unordered_set<std::string>{};
      seen.reserve(tags.size());

      for (auto const& tag : tags)
      {
        auto tagRes = detail::normalizeRuntimeText(tag, "Track tag");

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        if (seen.insert(*tagRes).second)
        {
          normalized.push_back(std::move(*tagRes));
        }
      }

      return normalized;
    }
  } // namespace

  namespace detail
  {
    Result<std::string> normalizeRuntimeText(std::string_view const value, std::string_view const context)
    {
      auto normalizedRes = utility::normalizeUtf8Nfc(value);

      if (!normalizedRes)
      {
        auto error = std::move(normalizedRes.error());
        error.message = std::format("{}: {}", context, error.message);
        return std::unexpected{std::move(error)};
      }

      return std::move(*normalizedRes);
    }

    std::unexpected<Error> storageError(char const* action, Error const& error)
    {
      return std::unexpected{Error{
        .code = error.code,
        .message = std::format("{}: {}", action, error.message),
        .location = error.location,
      }};
    }

    std::vector<TrackId> orderTrackIdsFrom(library::ListView const& view)
    {
      auto const trackIds = view.orderTrackIds();
      auto result = std::vector<TrackId>{};
      result.reserve(trackIds.size());
      result.append_range(trackIds);
      return result;
    }

    library::ListBuilder listWithOrder(library::ListView const& view, std::span<TrackId const> orderTrackIds)
    {
      auto builder = library::ListBuilder::fromView(view);
      builder.orderTrackIds().clear();

      for (auto const trackId : orderTrackIds)
      {
        builder.orderTrackIds().add(trackId);
      }

      return builder;
    }

    Result<library::ListView> requireList(library::ListWriter const& listWriter, ListId listId)
    {
      auto optView = listWriter.get(listId);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      return *optView;
    }

    delta::RegularTrackEditScript removalScriptFor(std::span<TrackId const> storedTrackIds,
                                                   std::unordered_set<TrackId> const& selectedTrackIds)
    {
      auto removals = std::vector<delta::RemoveRange>{};

      for (std::size_t index = 0; index < storedTrackIds.size(); ++index)
      {
        auto const trackId = storedTrackIds[index];

        if (!selectedTrackIds.contains(trackId))
        {
          continue;
        }

        if (removals.empty() || removals.back().start + removals.back().trackIds.size() != index)
        {
          removals.push_back(delta::RemoveRange{.start = index});
        }

        removals.back().trackIds.push_back(trackId);
      }

      std::ranges::reverse(removals);
      auto script = delta::RegularTrackEditScript{};
      script.edits.reserve(removals.size());

      for (auto& removal : removals)
      {
        script.edits.emplace_back(std::move(removal));
      }

      return script;
    }

    Result<EditTrackTagsReply> applyTagPatchInTransaction(library::MusicLibrary& library,
                                                          library::LibraryWrite& transaction,
                                                          std::span<TrackId const> trackIds,
                                                          std::span<std::string const> tagsToAdd,
                                                          std::span<std::string const> tagsToRemove)
    {
      auto normalizedAddRes = normalizeTags(tagsToAdd);

      if (!normalizedAddRes)
      {
        return std::unexpected{normalizedAddRes.error()};
      }

      auto normalizedRemoveRes = normalizeTags(tagsToRemove);

      if (!normalizedRemoveRes)
      {
        return std::unexpected{normalizedRemoveRes.error()};
      }

      auto writer = transaction.tracks();
      auto changes = std::vector<TrackTagsChange>{};

      for (auto const trackId : trackIds)
      {
        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromHotView(*optView, library.dictionary());
        auto& tags = builder.tags();
        bool changed = false;
        auto addedTags = std::vector<std::string>{};
        auto removedTags = std::vector<std::string>{};

        for (auto const& tag : *normalizedAddRes)
        {
          if (!std::ranges::contains(tags.names(), tag))
          {
            tags.add(tag);
            addedTags.push_back(tag);
            changed = true;
          }
        }

        for (auto const& tag : *normalizedRemoveRes)
        {
          if (std::ranges::contains(tags.names(), tag))
          {
            tags.remove(tag);
            removedTags.push_back(tag);
            changed = true;
          }
        }

        if (!changed)
        {
          continue;
        }

        if (auto result = writer.updateHot(trackId, builder); !result)
        {
          return storageError("Failed to update hot track data", result.error());
        }

        changes.push_back(TrackTagsChange{
          .trackId = trackId, .addedTags = std::move(addedTags), .removedTags = std::move(removedTags)});
      }

      return EditTrackTagsReply{.changes = std::move(changes)};
    }
  } // namespace detail

  namespace
  {
    template<typename Value, typename Owner, typename Operation>
    async::Task<Value> runWriterOperation(std::shared_ptr<Owner> ownerPtr,
                                          LibraryWriteLane::Submission submission,
                                          Operation operation)
    {
      auto optResult = std::optional<Value>{};
      auto deferredException = std::exception_ptr{};

      try
      {
        optResult.emplace(co_await operation(*ownerPtr, std::move(submission)));
      }
      catch (...)
      {
        deferredException = std::current_exception();
      }

      co_await ownerPtr->asyncRuntime.resumeOnCallbackExecutor();

      if (deferredException)
      {
        async::rethrowException(deferredException);
      }

      AO_INVARIANT(optResult);
      co_return std::move(*optResult);
    }

    // This ordinary function captures submission context before the returned
    // lazy coroutine can outlive its caller or cross an executor boundary.
    template<typename Value, typename Owner, typename Method, typename... Args>
    async::Task<Value> submitWriterOperation(std::shared_ptr<Owner> ownerPtr, Method method, Args... args)
    {
      auto submission = ownerPtr->writeLane.captureSubmission();
      return runWriterOperation<Value>(
        std::move(ownerPtr),
        std::move(submission),
        [method, ... args = std::move(args)](Owner& owner, LibraryWriteLane::Submission innerSubmission) mutable
        { return std::invoke(method, owner, std::move(innerSubmission), std::move(args)...); });
    }
  } // namespace

  LibraryCommands::LibraryCommands(library::MusicLibrary& library,
                                   LibraryWriteLane& writeLane,
                                   async::Runtime& asyncRuntime)
    : _implPtr{std::make_shared<Impl>(library, writeLane, asyncRuntime)}
  {
  }

  LibraryCommands::~LibraryCommands() = default;

  async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>> LibraryCommands::updateMetadata(
    BoundTrackTargets targets,
    MetadataPatch patch)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>>(
      _implPtr, &Impl::applyUpdateMetadata, std::move(targets), std::move(patch));
  }

  async::Task<Result<UpdateTrackMetadataReply>> LibraryCommands::previewUpdateMetadata(std::vector<TrackId> trackIds,
                                                                                       MetadataPatch patch)
  {
    return submitWriterOperation<Result<UpdateTrackMetadataReply>>(
      _implPtr, &Impl::previewUpdateMetadata, std::move(trackIds), std::move(patch));
  }

  async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> LibraryCommands::editTags(
    BoundTrackTargets targets,
    std::vector<std::string> tagsToAdd,
    std::vector<std::string> tagsToRemove)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<EditTrackTagsReply>>>(
      _implPtr, &Impl::applyEditTags, std::move(targets), std::move(tagsToAdd), std::move(tagsToRemove));
  }

  async::Task<Result<EditTrackTagsReply>> LibraryCommands::previewEditTags(std::vector<TrackId> trackIds,
                                                                           std::vector<std::string> tagsToAdd,
                                                                           std::vector<std::string> tagsToRemove)
  {
    return submitWriterOperation<Result<EditTrackTagsReply>>(
      _implPtr, &Impl::previewEditTags, std::move(trackIds), std::move(tagsToAdd), std::move(tagsToRemove));
  }

  async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>> LibraryCommands::updateProperties(
    BoundTrackTargets targets,
    TrackPropertiesPatch patch)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>>(
      _implPtr, &Impl::applyUpdateProperties, std::move(targets), std::move(patch));
  }

  async::Task<Result<TrackAuthoringResult<AddTracksToListReply>>> LibraryCommands::addTracksToList(
    ListId const listId,
    BoundTrackTargets targets)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<AddTracksToListReply>>>(
      _implPtr, &Impl::applyAddTracksToList, listId, std::move(targets));
  }

  async::Task<Result<AddTracksToListReply>> LibraryCommands::previewAddTracksToList(ListId const listId,
                                                                                    std::vector<TrackId> trackIds)
  {
    return submitWriterOperation<Result<AddTracksToListReply>>(
      _implPtr, &Impl::previewAddTracksToList, listId, std::move(trackIds));
  }

  async::Task<Result<TrackAuthoringResult<RemoveTracksFromListReply>>> LibraryCommands::removeTracksFromList(
    ListId const listId,
    BoundTrackTargets targets)
  {
    return submitWriterOperation<Result<TrackAuthoringResult<RemoveTracksFromListReply>>>(
      _implPtr, &Impl::applyRemoveTracksFromList, listId, std::move(targets));
  }

  async::Task<Result<RemoveTracksFromListReply>> LibraryCommands::previewRemoveTracksFromList(
    ListId const listId,
    std::vector<TrackId> trackIds)
  {
    return submitWriterOperation<Result<RemoveTracksFromListReply>>(
      _implPtr, &Impl::previewRemoveTracksFromList, listId, std::move(trackIds));
  }

  async::Task<Result<ListId>> LibraryCommands::createList(ListDraft draft)
  {
    return submitWriterOperation<Result<ListId>>(_implPtr, &Impl::createList, std::move(draft));
  }

  async::Task<Result<>> LibraryCommands::previewCreateList(ListDraft draft)
  {
    return submitWriterOperation<Result<>>(_implPtr, &Impl::previewCreateList, std::move(draft));
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::updateList(ListDraft draft)
  {
    return submitWriterOperation<Result<UpdateListReply>>(_implPtr, &Impl::updateList, std::move(draft));
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::previewUpdateList(ListDraft draft)
  {
    return submitWriterOperation<Result<UpdateListReply>>(_implPtr, &Impl::previewUpdateList, std::move(draft));
  }

  async::Task<Result<AuthoringResult<MoveListOrderReply>>> LibraryCommands::moveListOrder(
    BoundListOrder order,
    std::vector<TrackId> selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    return submitWriterOperation<Result<AuthoringResult<MoveListOrderReply>>>(
      _implPtr, &Impl::applyMoveListOrder, std::move(order), std::move(selectedTrackIds), optBeforeTrackId);
  }

  async::Task<Result<AuthoringResult<ResetListOrderReply>>> LibraryCommands::resetListOrder(BoundListOrder order)
  {
    return submitWriterOperation<Result<AuthoringResult<ResetListOrderReply>>>(
      _implPtr, &Impl::applyResetListOrder, std::move(order));
  }

  async::Task<Result<AuthoringResult<ForgetHiddenListOrderReply>>> LibraryCommands::forgetHiddenListOrder(
    BoundListOrder order)
  {
    return submitWriterOperation<Result<AuthoringResult<ForgetHiddenListOrderReply>>>(
      _implPtr, &Impl::applyForgetHiddenListOrder, std::move(order));
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::deleteList(ListId const listId, DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListReply>>(_implPtr, &Impl::deleteList, listId, options);
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::previewDeleteList(ListId const listId,
                                                                          DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListReply>>(_implPtr, &Impl::previewDeleteList, listId, options);
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::deleteListAndDescendants(ListId const listId,
                                                                                        DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListSubtreeReply>>(
      _implPtr, &Impl::deleteListAndDescendants, listId, options);
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::previewDeleteListAndDescendants(
    ListId const listId,
    DeleteListOptions const options)
  {
    return submitWriterOperation<Result<DeleteListSubtreeReply>>(
      _implPtr, &Impl::previewDeleteListAndDescendants, listId, options);
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::deleteTrack(TrackId const trackId)
  {
    return submitWriterOperation<Result<DeleteTrackReply>>(_implPtr, &Impl::deleteTrack, trackId);
  }

  async::Task<Result<DeleteTrackReply>> LibraryCommands::previewDeleteTrack(TrackId const trackId)
  {
    return submitWriterOperation<Result<DeleteTrackReply>>(_implPtr, &Impl::previewDeleteTrack, trackId);
  }

  async::Task<Result<CreateTrackReply>> LibraryCommands::createTrackFromFile(std::filesystem::path path)
  {
    return submitWriterOperation<Result<CreateTrackReply>>(_implPtr, &Impl::createTrackFromFile, std::move(path));
  }

  async::Task<Result<PreviewCreateTrackReply>> LibraryCommands::previewCreateTrackFromFile(std::filesystem::path path)
  {
    return submitWriterOperation<Result<PreviewCreateTrackReply>>(
      _implPtr, &Impl::previewCreateTrackFromFile, std::move(path));
  }
} // namespace ao::rt
