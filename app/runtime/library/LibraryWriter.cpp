// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryWriter.h>

#include "LibraryMutationService.h"
#include "MediaTrack.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/query/Field.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/utility/Path.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

    template<typename Setter>
    void applyStringPatch(std::optional<std::string> const& optValue,
                          std::string_view fieldName,
                          std::string_view current,
                          Setter setter,
                          bool& changed,
                          std::vector<TrackFieldChange>& changes)
    {
      if (!optValue || current == *optValue)
      {
        return;
      }

      changes.push_back(
        TrackFieldChange{.field = std::string{fieldName}, .oldValue = std::string{current}, .newValue = *optValue});
      setter(*optValue);
      changed = true;
    }

    template<typename Setter>
    void applyUint16Patch(std::optional<std::uint16_t> const& optValue,
                          std::string_view fieldName,
                          std::uint16_t current,
                          Setter setter,
                          bool& changed,
                          std::vector<TrackFieldChange>& changes)
    {
      if (!optValue || current == *optValue)
      {
        return;
      }

      changes.push_back(TrackFieldChange{.field = std::string{fieldName},
                                         .oldValue = std::format("{}", current),
                                         .newValue = std::format("{}", *optValue)});
      setter(*optValue);
      changed = true;
    }

    void applyTextMetadataPatch(library::TrackBuilder::MetadataBuilder& metadata,
                                MetadataPatch const& patch,
                                PatchResult& result,
                                std::vector<TrackFieldChange>& changes)
    {
      applyStringPatch(
        patch.optTitle,
        "title",
        metadata.title(),
        [&metadata](std::string_view value) { metadata.title(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optArtist,
        "artist",
        metadata.artist(),
        [&metadata](std::string_view value) { metadata.artist(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optAlbum,
        "album",
        metadata.album(),
        [&metadata](std::string_view value) { metadata.album(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optAlbumArtist,
        "albumArtist",
        metadata.albumArtist(),
        [&metadata](std::string_view value) { metadata.albumArtist(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optGenre,
        "genre",
        metadata.genre(),
        [&metadata](std::string_view value) { metadata.genre(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optComposer,
        "composer",
        metadata.composer(),
        [&metadata](std::string_view value) { metadata.composer(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optConductor,
        "conductor",
        metadata.conductor(),
        [&metadata](std::string_view value) { metadata.conductor(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optEnsemble,
        "ensemble",
        metadata.ensemble(),
        [&metadata](std::string_view value) { metadata.ensemble(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optWork,
        "work",
        metadata.work(),
        [&metadata](std::string_view value) { metadata.work(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optMovement,
        "movement",
        metadata.movement(),
        [&metadata](std::string_view value) { metadata.movement(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optSoloist,
        "soloist",
        metadata.soloist(),
        [&metadata](std::string_view value) { metadata.soloist(value); },
        result.changedCold,
        changes);
    }

    void applyNumberMetadataPatch(library::TrackBuilder::MetadataBuilder& metadata,
                                  MetadataPatch const& patch,
                                  PatchResult& result,
                                  std::vector<TrackFieldChange>& changes)
    {
      applyUint16Patch(
        patch.optYear,
        "year",
        metadata.year(),
        [&metadata](std::uint16_t value) { metadata.year(value); },
        result.changedHot,
        changes);
      applyUint16Patch(
        patch.optMovementNumber,
        "movementNumber",
        metadata.movementNumber(),
        [&metadata](std::uint16_t value) { metadata.movementNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optMovementTotal,
        "movementTotal",
        metadata.movementTotal(),
        [&metadata](std::uint16_t value) { metadata.movementTotal(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optTrackNumber,
        "trackNumber",
        metadata.trackNumber(),
        [&metadata](std::uint16_t value) { metadata.trackNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optTrackTotal,
        "trackTotal",
        metadata.trackTotal(),
        [&metadata](std::uint16_t value) { metadata.trackTotal(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optDiscNumber,
        "discNumber",
        metadata.discNumber(),
        [&metadata](std::uint16_t value) { metadata.discNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optDiscTotal,
        "discTotal",
        metadata.discTotal(),
        [&metadata](std::uint16_t value) { metadata.discTotal(value); },
        result.changedCold,
        changes);
    }

    void applyCustomMetadataPatch(library::TrackBuilder& builder,
                                  MetadataPatch const& patch,
                                  PatchResult& result,
                                  std::vector<TrackFieldChange>& changes)
    {
      for (auto const& [key, optValue] : patch.customUpdates)
      {
        if (key.empty())
        {
          continue;
        }

        auto const& pairs = builder.customMetadata().pairs();
        auto const existing =
          std::ranges::find_if(pairs, [&key](auto const& pair) { return pair.first == std::string_view{key}; });

        if (existing == pairs.end() && !optValue)
        {
          continue;
        }

        if (existing != pairs.end() && optValue && existing->second == *optValue)
        {
          continue;
        }

        changes.push_back(
          TrackFieldChange{.field = "custom." + key,
                           .oldValue = existing != pairs.end() ? std::string{existing->second} : std::string{},
                           .newValue = optValue ? *optValue : std::string{}});
        builder.customMetadata().remove(key);

        if (optValue)
        {
          builder.customMetadata().add(key, *optValue);
        }

        result.changedCold = true;
      }
    }

    PatchResult applyMetadataPatch(library::TrackBuilder& builder,
                                   MetadataPatch const& patch,
                                   std::vector<TrackFieldChange>& changes)
    {
      auto& metadata = builder.metadata();
      auto result = PatchResult{};

      applyTextMetadataPatch(metadata, patch, result, changes);
      applyNumberMetadataPatch(metadata, patch, result, changes);
      applyCustomMetadataPatch(builder, patch, result, changes);

      return result;
    }

    std::unexpected<Error> storageError(char const* action, Error const& error)
    {
      return std::unexpected{Error{
        .code = error.code,
        .message = std::format("{}: {}", action, error.message),
        .location = error.location,
      }};
    }

    std::unexpected<Error> prefixError(char const* prefix, Error const& error)
    {
      return std::unexpected{Error{
        .code = error.code,
        .message = std::format("{}: {}", prefix, error.message),
        .location = error.location,
      }};
    }

    template<typename Operation,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>>
    OperationResult applyInteractivePreview(LibraryMutationService& mutationService, Operation&& operation)
    {
      auto mutationRes = mutationService.beginInteractiveMutation();

      if (!mutationRes)
      {
        return OperationResult{std::unexpected{mutationRes.error()}};
      }

      auto mutation = std::move(*mutationRes);
      auto result = mutation.apply(std::forward<Operation>(operation));

      if (result)
      {
        mutation.abort();
      }

      return result;
    }

    template<typename Operation,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>,
             typename Value = detail::OperationResultTraits<OperationResult>::ValueType>
    Result<MutationExecution<Value>> executeInteractiveMutation(LibraryMutationService& mutationService,
                                                                std::string_view operationName,
                                                                Operation&& operation)
    {
      auto mutationRes = mutationService.beginInteractiveMutation();

      if (!mutationRes)
      {
        return std::unexpected{mutationRes.error()};
      }

      auto mutation = std::move(*mutationRes);
      return mutation.execute(std::forward<Operation>(operation), operationName);
    }

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

    Result<> validateListExpression(std::string const& expression)
    {
      auto exprRes = query::parse(expression.empty() ? "true" : expression);

      if (!exprRes)
      {
        return prefixError("invalid list filter", exprRes.error());
      }

      if (auto planRes = query::compileQuery(*exprRes); !planRes)
      {
        return prefixError("invalid list filter", planRes.error());
      }

      return {};
    }

    library::ListBuilder listForDraft(LibraryWriter::ListDraft const& draft,
                                      std::optional<library::ListView> const& optExisting = std::nullopt)
    {
      auto builder = optExisting ? library::ListBuilder::fromView(*optExisting) : library::ListBuilder::makeEmpty();
      builder.name(draft.name).description(draft.description).filter(draft.expression).parentId(draft.parentId);
      return builder;
    }

    Result<> validateListDraft(LibraryWriter::ListDraft const& draft)
    {
      return validateListExpression(draft.expression);
    }

    std::vector<TrackId> orderTrackIdsFrom(library::ListView::OrderTrackIdProxy const& trackIds)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(trackIds.size());
      result.append_range(trackIds);
      return result;
    }

    std::vector<TrackId> orderTrackIdsFrom(library::ListView const& view)
    {
      return orderTrackIdsFrom(view.orderTrackIds());
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

          auto const storedTrackIds = orderTrackIdsFrom(view);
          auto const selectedTrackIds = std::unordered_set<TrackId>{trackId};
          auto builder = library::ListBuilder::fromView(view);
          builder.orderTrackIds().remove(trackId);

          updates.push_back(PendingListOrderRemoval{
            .listId = listId,
            .list = std::move(builder),
            .orderChange =
              ListOrderChange{
                .listId = listId,
                .operation = removalScriptFor(storedTrackIds, selectedTrackIds),
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
          return storageError("Failed to remove deleted track from List order", updateRes.error());
        }

        result.changedListIds.push_back(update.listId);
        result.orderChanges.push_back(std::move(update.orderChange));
      }

      return result;
    }

    void appendListFieldChange(std::vector<ListFieldChange>& changes,
                               std::string_view field,
                               std::string_view oldValue,
                               std::string_view newValue)
    {
      if (oldValue == newValue)
      {
        return;
      }

      changes.push_back(ListFieldChange{
        .field = std::string{field}, .oldValue = std::string{oldValue}, .newValue = std::string{newValue}});
    }

    UpdateListReply diffListUpdate(library::ListView const& existing, LibraryWriter::ListDraft const& draft)
    {
      auto reply = UpdateListReply{};
      appendListFieldChange(reply.fieldChanges, "name", existing.name(), draft.name);
      appendListFieldChange(reply.fieldChanges, "description", existing.description(), draft.description);
      appendListFieldChange(reply.fieldChanges,
                            "parentId",
                            std::format("{}", existing.parentId().raw()),
                            std::format("{}", draft.parentId.raw()));
      appendListFieldChange(reply.fieldChanges, "filter", existing.filter(), draft.expression);

      return reply;
    }

    Result<ListId> createListInTransaction(library::LibraryWrite& transaction, LibraryWriter::ListDraft const& draft)
    {
      auto listWriter = transaction.lists();
      auto list = listForDraft(draft);

      if (auto result = validateListDraft(draft); !result)
      {
        return std::unexpected{result.error()};
      }

      auto result = listWriter.create(list);

      if (!result)
      {
        return storageError("Failed to create list", result.error());
      }

      return *result;
    }

    Result<UpdateListReply> updateListInTransaction(library::LibraryWrite& transaction,
                                                    LibraryWriter::ListDraft const& draft)
    {
      auto listWriter = transaction.lists();
      auto optExisting = listWriter.get(draft.listId);

      if (!optExisting)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", draft.listId));
      }

      auto list = listForDraft(draft, optExisting);

      if (auto result = validateListDraft(draft); !result)
      {
        return std::unexpected{result.error()};
      }

      auto reply = diffListUpdate(*optExisting, draft);

      if (reply.fieldChanges.empty())
      {
        return reply;
      }

      reply.changed = true;

      if (auto result = listWriter.update(draft.listId, list); !result)
      {
        return storageError("Failed to update list", result.error());
      }

      return reply;
    }

    Result<UpdateTrackMetadataReply> applyMetadataPatchInTransaction(library::MusicLibrary& library,
                                                                     library::LibraryWrite& transaction,
                                                                     std::span<TrackId const> trackIds,
                                                                     MetadataPatch const& patch)
    {
      auto writer = transaction.tracks();
      auto changes = std::vector<TrackChangeRecord>{};

      for (auto const trackId : trackIds)
      {
        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromCompleteView(*optView, library.dictionary());
        auto fieldChanges = std::vector<TrackFieldChange>{};
        auto const patchRes = applyMetadataPatch(builder, patch, fieldChanges);

        if (!patchRes.changedHot && !patchRes.changedCold)
        {
          continue;
        }

        auto updateRes = Result<>{};

        if (patchRes.changedHot && patchRes.changedCold)
        {
          updateRes = writer.update(trackId, builder);
        }
        else if (patchRes.changedHot)
        {
          updateRes = writer.updateHot(trackId, builder);
        }
        else
        {
          updateRes = writer.updateCold(trackId, builder);
        }

        if (!updateRes)
        {
          return storageError("Failed to update track data", updateRes.error());
        }

        changes.push_back(TrackChangeRecord{.trackId = trackId, .fields = std::move(fieldChanges)});
      }

      return UpdateTrackMetadataReply{.changes = std::move(changes)};
    }

    Result<EditTrackTagsReply> applyTagPatchInTransaction(library::MusicLibrary& library,
                                                          library::LibraryWrite& transaction,
                                                          std::span<TrackId const> trackIds,
                                                          std::span<std::string const> tagsToAdd,
                                                          std::span<std::string const> tagsToRemove)
    {
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

        for (auto const& tag : tagsToAdd)
        {
          if (!std::ranges::contains(tags.names(), tag))
          {
            tags.add(tag);
            addedTags.push_back(tag);
            changed = true;
          }
        }

        for (auto const& tag : tagsToRemove)
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

    struct DeleteListTagImpactWork final
    {
      DeleteListReply::TagImpact impact{};
      std::vector<TrackId> taggedTrackIds{};
    };

    Result<std::optional<DeleteListTagImpactWork>> analyzeDeleteListTagImpact(
      library::MusicLibrary& library,
      library::LibraryWrite& transaction,
      library::ListView const& targetView,
      std::span<ListId const> const deletedListIds)
    {
      auto const optTag = writableTagForListExpression(targetView.filter());

      if (!optTag)
      {
        return std::optional<DeleteListTagImpactWork>{};
      }

      auto work = DeleteListTagImpactWork{};
      work.impact.tag = *optTag;
      auto const deleted = std::unordered_set<ListId>{deletedListIds.begin(), deletedListIds.end()};

      for (auto const& [listId, view] : library.lists().reader(transaction))
      {
        if (!deleted.contains(listId) && listExpressionReferencesTag(view.filter(), *optTag))
        {
          work.impact.otherListReferences.push_back(
            DeleteListReply::TagReference{.listId = listId, .name = std::string{view.name()}});
        }
      }

      auto const& dictionary = library.dictionary();

      for (auto const& [trackId, view] : library.tracks().reader(transaction).hot())
      {
        bool hasTag = false;

        for (auto const tagId : view.tags())
        {
          if (dictionary.get(tagId) == *optTag)
          {
            hasTag = true;
            break;
          }
        }

        if (hasTag)
        {
          work.taggedTrackIds.push_back(trackId);
        }
      }

      work.impact.taggedTrackCount = work.taggedTrackIds.size();
      return std::optional<DeleteListTagImpactWork>{std::move(work)};
    }

    Result<std::vector<DeleteListReply>> collectDeleteListSubtree(library::MusicLibrary& library,
                                                                  library::LibraryWrite& transaction,
                                                                  ListId const rootListId)
    {
      auto records = std::unordered_map<ListId, DeleteListReply>{};
      auto childIdsByParent = std::unordered_map<ListId, std::vector<ListId>>{};

      for (auto const& [savedListId, view] : library.lists().reader(transaction))
      {
        records.emplace(savedListId,
                        DeleteListReply{
                          .listId = savedListId,
                          .name = std::string{view.name()},
                          .orderTrackIdCount = view.orderTrackIds().size(),
                        });
        childIdsByParent[view.parentId()].push_back(savedListId);
      }

      if (!records.contains(rootListId))
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", rootListId));
      }

      auto deletedLists = std::vector<DeleteListReply>{};
      auto visiting = std::unordered_set<ListId>{};
      auto visited = std::unordered_set<ListId>{};
      auto visit = std::function<Result<>(ListId)>{};
      visit = [&](ListId const currentId) -> Result<>
      {
        if (visited.contains(currentId))
        {
          return {};
        }

        if (!visiting.insert(currentId).second)
        {
          return makeError(Error::Code::InvalidState, "List cycle detected while previewing subtree deletion");
        }

        auto const record = records.find(currentId);

        if (record == records.end())
        {
          return makeError(Error::Code::InvalidState, "List disappeared while previewing subtree deletion");
        }

        deletedLists.push_back(record->second);

        if (auto const children = childIdsByParent.find(currentId); children != childIdsByParent.end())
        {
          for (auto const childId : children->second)
          {
            if (auto result = visit(childId); !result)
            {
              return result;
            }
          }
        }

        visiting.erase(currentId);
        visited.insert(currentId);
        return {};
      };

      if (auto result = visit(rootListId); !result)
      {
        return std::unexpected{result.error()};
      }

      return deletedLists;
    }

    Result<std::string> requireWritableListTag(library::ListView const& view, ListId const listId)
    {
      auto optTag = writableTagForListExpression(view.filter());

      if (!optTag)
      {
        return makeError(
          Error::Code::InvalidInput,
          std::format("List {} membership is computed; its expression must be exactly one positive tag", listId));
      }

      return std::move(*optTag);
    }

    struct ParentFilterPlan final
    {
      ListId listId = kInvalidListId;
      query::ExecutionPlan plan{};
    };

    Error storedParentFilterError(ListId const parentId, Error error)
    {
      error.code = Error::Code::FormatRejected;
      error.message = std::format("invalid stored filter for parent List {}: {}", parentId, error.message);
      return error;
    }

    Result<std::vector<ParentFilterPlan>> compileParentFilterPlans(library::MusicLibrary& library,
                                                                   library::LibraryWrite& transaction,
                                                                   library::ListView const& targetView)
    {
      auto plans = std::vector<ParentFilterPlan>{};
      auto reader = library.lists().reader(transaction);
      auto parentId = targetView.parentId();
      auto visited = std::unordered_set<ListId>{};

      while (parentId != kInvalidListId)
      {
        auto const inserted = visited.insert(parentId).second;
        AO_INVARIANT(inserted, "List parent cycle detected while validating membership");

        auto optParent = reader.get(parentId);

        AO_INVARIANT(optParent, "List parent is missing while validating membership");

        if (!optParent->filter().empty())
        {
          auto expressionRes = query::parse(optParent->filter());

          if (!expressionRes)
          {
            return std::unexpected{storedParentFilterError(parentId, std::move(expressionRes).error())};
          }

          auto planRes = query::compileQuery(*expressionRes);

          if (!planRes)
          {
            return std::unexpected{storedParentFilterError(parentId, std::move(planRes).error())};
          }

          plans.push_back(ParentFilterPlan{.listId = parentId, .plan = std::move(*planRes)});
        }

        parentId = optParent->parentId();
      }

      return plans;
    }

    Result<> validateTracksExist(library::MusicLibrary& library,
                                 library::LibraryWrite& transaction,
                                 std::span<TrackId const> trackIds)
    {
      auto reader = library.tracks().reader(transaction);

      for (auto const trackId : trackIds)
      {
        if (!reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }
      }

      return {};
    }

    Result<> validateParentMembership(library::MusicLibrary& library,
                                      library::LibraryWrite& transaction,
                                      library::ListView const& targetView,
                                      ListId const targetListId,
                                      std::span<TrackId const> trackIds)
    {
      auto plansRes = compileParentFilterPlans(library, transaction, targetView);

      if (!plansRes)
      {
        return std::unexpected{plansRes.error()};
      }

      auto& plans = *plansRes;

      if (plans.empty())
      {
        return validateTracksExist(library, transaction, trackIds);
      }

      auto dictionaryCache = library::DictionaryReadCache{library.dictionary()};
      auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
      auto bindings = std::vector<query::PlanBinding>{};
      bindings.reserve(plans.size());

      for (auto const& plan : plans)
      {
        bindings.emplace_back(plan.plan, dictionaryContext);
      }

      auto reader = library.tracks().reader(transaction);
      auto evaluator = query::PlanEvaluator{};

      for (auto const trackId : trackIds)
      {
        auto optTrack = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optTrack)
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }

        for (std::size_t index = 0; index < plans.size(); ++index)
        {
          AO_INVARIANT(query::hasRequiredTrackData(plans[index].plan.accessProfile, *optTrack),
                       "Complete Track {} lacks data required by parent List {}",
                       trackId,
                       plans[index].listId);

          if (!evaluator.matches(bindings[index], *optTrack))
          {
            return makeError(
              Error::Code::InvalidInput,
              std::format(
                "Track {} is outside parent List {} of target List {}", trackId, plans[index].listId, targetListId));
          }
        }
      }

      return {};
    }

    Result<AddTracksToListReply> applyAddTracksToListInTransaction(library::MusicLibrary& library,
                                                                   library::LibraryWrite& transaction,
                                                                   ListId const listId,
                                                                   std::span<TrackId const> trackIds)
    {
      auto listName = std::string{};
      auto tag = std::string{};

      // LMDB values are borrowed from the transaction and may be invalidated by
      // a later write. Materialize every List field used by the reply before
      // applyTagPatchInTransaction performs any database mutation.
      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, listId);

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto tagRes = requireWritableListTag(*viewRes, listId);

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        if (auto eligibilityRes = validateParentMembership(library, transaction, *viewRes, listId, trackIds);
            !eligibilityRes)
        {
          return std::unexpected{eligibilityRes.error()};
        }

        listName = std::string{viewRes->name()};
        tag = std::move(*tagRes);
      }

      auto tags = std::array<std::string, 1>{tag};
      auto editRes = applyTagPatchInTransaction(library, transaction, trackIds, tags, {});

      if (!editRes)
      {
        return std::unexpected{editRes.error()};
      }

      return AddTracksToListReply{
        .listId = listId,
        .listName = std::move(listName),
        .tag = std::move(tag),
        .targetTrackIds = {trackIds.begin(), trackIds.end()},
        .tagEdit = std::move(*editRes),
      };
    }

    struct RemoveTracksFromListWork final
    {
      RemoveTracksFromListReply reply{};
      std::optional<ListOrderChange> optOrderChange{};
    };

    Result<RemoveTracksFromListWork> applyRemoveTracksFromListInTransaction(library::MusicLibrary& library,
                                                                            library::LibraryWrite& transaction,
                                                                            ListId const listId,
                                                                            std::span<TrackId const> trackIds)
    {
      if (auto existenceRes = validateTracksExist(library, transaction, trackIds); !existenceRes)
      {
        return std::unexpected{existenceRes.error()};
      }

      auto const selected = std::unordered_set<TrackId>{trackIds.begin(), trackIds.end()};
      auto listName = std::string{};
      auto tag = std::string{};
      auto oldOrder = std::vector<TrackId>{};
      auto nextOrder = std::vector<TrackId>{};
      auto forgotten = std::vector<TrackId>{};
      auto optList = std::optional<library::ListBuilder>{};

      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, listId);

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto tagRes = requireWritableListTag(*viewRes, listId);

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        listName = std::string{viewRes->name()};
        tag = std::move(*tagRes);
        oldOrder = orderTrackIdsFrom(*viewRes);
        nextOrder = oldOrder;

        for (auto const trackId : oldOrder)
        {
          if (selected.contains(trackId))
          {
            forgotten.push_back(trackId);
          }
        }

        std::erase_if(nextOrder, [&selected](TrackId const trackId) { return selected.contains(trackId); });

        if (nextOrder != oldOrder)
        {
          optList.emplace(listWithOrder(*viewRes, nextOrder));
        }
      }

      auto tags = std::array<std::string, 1>{tag};
      auto editRes = applyTagPatchInTransaction(library, transaction, trackIds, {}, tags);

      if (!editRes)
      {
        return std::unexpected{editRes.error()};
      }

      auto work = RemoveTracksFromListWork{
        .reply =
          RemoveTracksFromListReply{
            .listId = listId,
            .listName = std::move(listName),
            .tag = std::move(tag),
            .targetTrackIds = {trackIds.begin(), trackIds.end()},
            .tagEdit = std::move(*editRes),
            .forgottenPositionTrackIds = std::move(forgotten),
          },
      };

      if (nextOrder == oldOrder)
      {
        return work;
      }

      AO_INVARIANT(optList, "Changed List order did not retain its semantic update");

      if (auto updateRes = transaction.lists().update(listId, *optList); !updateRes)
      {
        return storageError("Failed to forget removed List positions", updateRes.error());
      }

      work.optOrderChange = ListOrderChange{
        .listId = listId,
        .operation = removalScriptFor(oldOrder, selected),
      };
      return work;
    }

    Result<LibraryChangeSet> applyMoveListOrderInTransaction(library::LibraryWrite& transaction,
                                                             BoundListOrder const& order,
                                                             std::span<TrackId const> selectedTrackIds,
                                                             std::span<TrackId const> desiredEffectiveTrackIds,
                                                             std::optional<TrackId> const optBeforeTrackId)
    {
      auto listWriter = transaction.lists();
      auto viewRes = requireList(listWriter, order.listId());

      if (!viewRes)
      {
        return std::unexpected{viewRes.error()};
      }

      auto const& view = *viewRes;
      auto const oldOrderTrackIds = orderTrackIdsFrom(view);
      auto nextOrderTrackIds = oldOrderTrackIds;
      auto rankedMembership = std::unordered_set<TrackId>{nextOrderTrackIds.begin(), nextOrderTrackIds.end()};
      auto const effectiveTrackIds = order.effectiveTrackIds();
      auto const effectiveMembership = std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
      auto const selectedMembership = std::unordered_set<TrackId>{selectedTrackIds.begin(), selectedTrackIds.end()};

      for (auto const trackId : effectiveTrackIds)
      {
        if (rankedMembership.insert(trackId).second)
        {
          nextOrderTrackIds.push_back(trackId);
        }
      }

      std::erase_if(nextOrderTrackIds,
                    [&selectedMembership](TrackId const trackId) { return selectedMembership.contains(trackId); });
      auto orderInsertion = nextOrderTrackIds.end();

      if (optBeforeTrackId)
      {
        orderInsertion = std::ranges::find(nextOrderTrackIds, *optBeforeTrackId);

        AO_INVARIANT(
          orderInsertion != nextOrderTrackIds.end(), "Bound List order anchor is absent from materialized order");
      }

      nextOrderTrackIds.insert(orderInsertion, selectedTrackIds.begin(), selectedTrackIds.end());

      auto projectedOrder = std::vector<TrackId>{};
      projectedOrder.reserve(effectiveTrackIds.size());

      for (auto const trackId : nextOrderTrackIds)
      {
        if (effectiveMembership.contains(trackId))
        {
          projectedOrder.push_back(trackId);
        }
      }

      AO_INVARIANT(std::ranges::equal(projectedOrder, desiredEffectiveTrackIds),
                   "Materialized List order does not represent the requested move");

      auto list = listWithOrder(view, nextOrderTrackIds);

      if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
      {
        return storageError("Failed to update List order", updateRes.error());
      }

      auto script = delta::diff(oldOrderTrackIds, nextOrderTrackIds, {}, selectedTrackIds);
      return LibraryChangeSet{.listsUpserted = {order.listId()},
                              .listOrderChanges = {
                                ListOrderChange{.listId = order.listId(), .operation = std::move(script)},
                              }};
    }

    struct DeleteListWork final
    {
      DeleteListReply reply{};
      LibraryChangeSet changeSet{};
    };

    Result<DeleteListWork> applyDeleteListInTransaction(library::MusicLibrary& library,
                                                        library::LibraryWrite& transaction,
                                                        ListId const listId,
                                                        DeleteListOptions const options)
    {
      auto listWriter = transaction.lists();
      auto optView = listWriter.get(listId);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      auto dependentDescriptions = std::string{};

      for (auto const& [dependentId, dependentView] : library.lists().reader(transaction))
      {
        if (dependentView.parentId() == listId)
        {
          if (!dependentDescriptions.empty())
          {
            dependentDescriptions.append(", ");
          }

          dependentDescriptions.append(std::format("{} ({})", dependentView.name(), dependentId));
        }
      }

      if (!dependentDescriptions.empty())
      {
        return makeError(
          Error::Code::Conflict, std::format("List {} has dependent Lists: {}", listId, dependentDescriptions));
      }

      auto work = DeleteListWork{
        .reply =
          DeleteListReply{
            .listId = listId,
            .name = std::string{optView->name()},
            .orderTrackIdCount = optView->orderTrackIds().size(),
          },
      };
      auto const deletedListIds = std::array{listId};
      auto tagImpactWorkRes = analyzeDeleteListTagImpact(library, transaction, *optView, deletedListIds);

      if (!tagImpactWorkRes)
      {
        return std::unexpected{tagImpactWorkRes.error()};
      }

      auto optTagImpactWork = std::move(*tagImpactWorkRes);

      if (auto removeRes = listWriter.remove(listId); !removeRes)
      {
        return std::unexpected{removeRes.error()};
      }

      auto mutatedTrackIds = std::vector<TrackId>{};

      if (options.removeWritableTagFromTracks)
      {
        if (!optTagImpactWork)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("List {} does not have directly editable tag membership", listId));
        }

        auto& tagImpactWork = *optTagImpactWork;
        auto const tags = std::array{tagImpactWork.impact.tag};
        auto tagEditRes = applyTagPatchInTransaction(library, transaction, tagImpactWork.taggedTrackIds, {}, tags);

        if (!tagEditRes)
        {
          return std::unexpected{tagEditRes.error()};
        }

        tagImpactWork.impact.removedFromTrackCount = tagEditRes->changes.size();
        mutatedTrackIds =
          tagEditRes->changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
      }

      if (optTagImpactWork)
      {
        work.reply.optTagImpact = optTagImpactWork->impact;
      }

      work.changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedTrackIds), .listsDeleted = {listId}};
      return work;
    }

    struct DeleteListSubtreeWork final
    {
      DeleteListSubtreeReply reply{};
      LibraryChangeSet changeSet{};
    };

    Result<DeleteListSubtreeWork> applyDeleteListSubtreeInTransaction(library::MusicLibrary& library,
                                                                      library::LibraryWrite& transaction,
                                                                      ListId const listId,
                                                                      DeleteListOptions const options)
    {
      auto deletedListsRes = collectDeleteListSubtree(library, transaction, listId);

      if (!deletedListsRes)
      {
        return std::unexpected{deletedListsRes.error()};
      }

      auto work = DeleteListSubtreeWork{
        .reply = DeleteListSubtreeReply{.rootListId = listId, .deletedLists = std::move(*deletedListsRes)},
      };
      auto deletedIds =
        work.reply.deletedLists | std::views::transform(&DeleteListReply::listId) | std::ranges::to<std::vector>();
      auto optRootView = library.lists().reader(transaction).get(listId);

      if (!optRootView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      auto tagImpactWorkRes = analyzeDeleteListTagImpact(library, transaction, *optRootView, deletedIds);

      if (!tagImpactWorkRes)
      {
        return std::unexpected{tagImpactWorkRes.error()};
      }

      auto optTagImpactWork = std::move(*tagImpactWorkRes);
      auto listWriter = transaction.lists();
      auto removedIdsRes = listWriter.removeSubtree(listId);

      if (!removedIdsRes)
      {
        return std::unexpected{removedIdsRes.error()};
      }

      AO_INVARIANT(std::ranges::is_permutation(*removedIdsRes, deletedIds),
                   "Logical List subtree membership disagreed with the application snapshot");

      auto mutatedTrackIds = std::vector<TrackId>{};

      if (options.removeWritableTagFromTracks)
      {
        if (!optTagImpactWork)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("List {} does not have directly editable tag membership", listId));
        }

        auto& tagImpactWork = *optTagImpactWork;
        auto const tags = std::array{tagImpactWork.impact.tag};
        auto tagEditRes = applyTagPatchInTransaction(library, transaction, tagImpactWork.taggedTrackIds, {}, tags);

        if (!tagEditRes)
        {
          return std::unexpected{tagEditRes.error()};
        }

        tagImpactWork.impact.removedFromTrackCount = tagEditRes->changes.size();
        mutatedTrackIds =
          tagEditRes->changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
      }

      if (optTagImpactWork)
      {
        work.reply.deletedLists.front().optTagImpact = optTagImpactWork->impact;
      }

      work.changeSet =
        LibraryChangeSet{.tracksMutated = std::move(mutatedTrackIds), .listsDeleted = std::move(deletedIds)};
      return work;
    }

    struct DeleteTrackWork final
    {
      DeleteTrackReply reply{};
      LibraryChangeSet changeSet{};
    };

    Result<DeleteTrackWork> applyDeleteTrackInTransaction(library::MusicLibrary& library,
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
      auto work = DeleteTrackWork{
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
        return storageError("Failed to create track data", createRes.error());
      }

      return CreateTrackFacts{
        .provisionalTrackId = *createRes,
        .uri = target.uri,
        .title = title,
        .artist = artist,
      };
    }
  } // namespace

  struct LibraryWriter::Impl final
  {
    Result<UpdateTrackMetadataReply> previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                           MetadataPatch const& patch);
    Result<MetadataAuthoringResult> applyUpdateMetadata(BoundTrackTargets const& targets, MetadataPatch const& patch);
    Result<EditTrackTagsReply> previewEditTags(std::span<TrackId const> trackIds,
                                               std::span<std::string const> tagsToAdd,
                                               std::span<std::string const> tagsToRemove);
    Result<TagAuthoringResult> applyEditTags(BoundTrackTargets const& targets,
                                             std::span<std::string const> tagsToAdd,
                                             std::span<std::string const> tagsToRemove);
    Result<AddTracksToListReply> previewAddTracksToList(ListId listId, std::span<TrackId const> trackIds);
    Result<AddToListAuthoringResult> applyAddTracksToList(ListId listId, BoundTrackTargets const& targets);
    Result<RemoveTracksFromListReply> previewRemoveTracksFromList(ListId listId, std::span<TrackId const> trackIds);
    Result<RemoveFromListAuthoringResult> applyRemoveTracksFromList(ListId listId, BoundTrackTargets const& targets);
    Result<ListId> createList(ListDraft const& draft);
    Result<> previewCreateList(ListDraft const& draft);
    Result<UpdateListReply> updateList(ListDraft const& draft);
    Result<UpdateListReply> previewUpdateList(ListDraft const& draft);
    Result<MoveOrderAuthoringResult> applyMoveListOrder(BoundListOrder const& order,
                                                        std::span<TrackId const> selectedTrackIds,
                                                        std::optional<TrackId> optBeforeTrackId);
    Result<ResetOrderAuthoringResult> applyResetListOrder(BoundListOrder const& order);
    Result<ForgetHiddenOrderAuthoringResult> applyForgetHiddenListOrder(BoundListOrder const& order);
    Result<DeleteListReply> deleteList(ListId listId, DeleteListOptions options);
    Result<DeleteListReply> previewDeleteList(ListId listId, DeleteListOptions options);
    Result<DeleteListSubtreeReply> deleteListAndDescendants(ListId listId, DeleteListOptions options);
    Result<DeleteListSubtreeReply> previewDeleteListAndDescendants(ListId listId, DeleteListOptions options);
    Result<DeleteTrackReply> deleteTrack(TrackId trackId);
    Result<DeleteTrackReply> previewDeleteTrack(TrackId trackId);
    Result<CreateTrackReply> createTrackFromFile(std::filesystem::path const& path);
    Result<PreviewCreateTrackReply> previewCreateTrackFromFile(std::filesystem::path const& path);

    library::MusicLibrary& library;
    LibraryMutationService& mutationService;
  };

  LibraryWriter::LibraryWriter(library::MusicLibrary& library, LibraryMutationService& mutationService)
    : _implPtr{std::make_unique<Impl>(library, mutationService)}
  {
  }

  LibraryWriter::~LibraryWriter() = default;

  Result<LibraryWriter::MetadataAuthoringResult> LibraryWriter::updateMetadata(BoundTrackTargets const& targets,
                                                                               MetadataPatch const& patch)
  {
    return _implPtr->applyUpdateMetadata(targets, patch);
  }

  Result<UpdateTrackMetadataReply> LibraryWriter::previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                                        MetadataPatch const& patch)
  {
    return _implPtr->previewUpdateMetadata(trackIds, patch);
  }

  Result<LibraryWriter::TagAuthoringResult> LibraryWriter::editTags(BoundTrackTargets const& targets,
                                                                    std::span<std::string const> tagsToAdd,
                                                                    std::span<std::string const> tagsToRemove)
  {
    return _implPtr->applyEditTags(targets, tagsToAdd, tagsToRemove);
  }

  Result<EditTrackTagsReply> LibraryWriter::previewEditTags(std::span<TrackId const> trackIds,
                                                            std::span<std::string const> tagsToAdd,
                                                            std::span<std::string const> tagsToRemove)
  {
    return _implPtr->previewEditTags(trackIds, tagsToAdd, tagsToRemove);
  }

  Result<LibraryWriter::AddToListAuthoringResult> LibraryWriter::addTracksToList(ListId const listId,
                                                                                 BoundTrackTargets const& targets)
  {
    return _implPtr->applyAddTracksToList(listId, targets);
  }

  Result<AddTracksToListReply> LibraryWriter::previewAddTracksToList(ListId const listId,
                                                                     std::span<TrackId const> const trackIds)
  {
    return _implPtr->previewAddTracksToList(listId, trackIds);
  }

  Result<LibraryWriter::RemoveFromListAuthoringResult> LibraryWriter::removeTracksFromList(
    ListId const listId,
    BoundTrackTargets const& targets)
  {
    return _implPtr->applyRemoveTracksFromList(listId, targets);
  }

  Result<RemoveTracksFromListReply> LibraryWriter::previewRemoveTracksFromList(ListId const listId,
                                                                               std::span<TrackId const> const trackIds)
  {
    return _implPtr->previewRemoveTracksFromList(listId, trackIds);
  }

  Result<ListId> LibraryWriter::createList(ListDraft const& draft)
  {
    return _implPtr->createList(draft);
  }

  Result<> LibraryWriter::previewCreateList(ListDraft const& draft)
  {
    return _implPtr->previewCreateList(draft);
  }

  Result<UpdateListReply> LibraryWriter::updateList(ListDraft const& draft)
  {
    return _implPtr->updateList(draft);
  }

  Result<UpdateListReply> LibraryWriter::previewUpdateList(ListDraft const& draft)
  {
    return _implPtr->previewUpdateList(draft);
  }

  Result<LibraryWriter::MoveOrderAuthoringResult> LibraryWriter::moveListOrder(
    BoundListOrder const& order,
    std::span<TrackId const> const selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    return _implPtr->applyMoveListOrder(order, selectedTrackIds, optBeforeTrackId);
  }

  Result<LibraryWriter::ResetOrderAuthoringResult> LibraryWriter::resetListOrder(BoundListOrder const& order)
  {
    return _implPtr->applyResetListOrder(order);
  }

  Result<LibraryWriter::ForgetHiddenOrderAuthoringResult> LibraryWriter::forgetHiddenListOrder(
    BoundListOrder const& order)
  {
    return _implPtr->applyForgetHiddenListOrder(order);
  }

  Result<DeleteListReply> LibraryWriter::deleteList(ListId listId, DeleteListOptions const options)
  {
    return _implPtr->deleteList(listId, options);
  }

  Result<DeleteListReply> LibraryWriter::previewDeleteList(ListId listId, DeleteListOptions const options)
  {
    return _implPtr->previewDeleteList(listId, options);
  }

  Result<DeleteListSubtreeReply> LibraryWriter::deleteListAndDescendants(ListId const listId,
                                                                         DeleteListOptions const options)
  {
    return _implPtr->deleteListAndDescendants(listId, options);
  }

  Result<DeleteListSubtreeReply> LibraryWriter::previewDeleteListAndDescendants(ListId const listId,
                                                                                DeleteListOptions const options)
  {
    return _implPtr->previewDeleteListAndDescendants(listId, options);
  }

  Result<DeleteTrackReply> LibraryWriter::deleteTrack(TrackId trackId)
  {
    return _implPtr->deleteTrack(trackId);
  }

  Result<DeleteTrackReply> LibraryWriter::previewDeleteTrack(TrackId trackId)
  {
    return _implPtr->previewDeleteTrack(trackId);
  }

  Result<CreateTrackReply> LibraryWriter::createTrackFromFile(std::filesystem::path const& path)
  {
    return _implPtr->createTrackFromFile(path);
  }

  Result<PreviewCreateTrackReply> LibraryWriter::previewCreateTrackFromFile(std::filesystem::path const& path)
  {
    return _implPtr->previewCreateTrackFromFile(path);
  }

  Result<UpdateTrackMetadataReply> LibraryWriter::Impl::previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                                              MetadataPatch const& patch)
  {
    return applyInteractivePreview(mutationService,
                                   [this, trackIds, &patch](library::LibraryWrite& transaction)
                                   { return applyMetadataPatchInTransaction(library, transaction, trackIds, patch); });
  }

  Result<LibraryWriter::MetadataAuthoringResult> LibraryWriter::Impl::applyUpdateMetadata(
    BoundTrackTargets const& targets,
    MetadataPatch const& patch)
  {
    auto start = mutationService.beginAuthoringMutation(targets);
    auto result = MetadataAuthoringResult{.status = start.status};

    if (!start.optMutation)
    {
      return result;
    }

    auto executionRes = start.optMutation->execute(
      [this, &targets, &patch](library::LibraryWrite& transaction) -> Result<OperationOutcome<UpdateTrackMetadataReply>>
      {
        auto replyRes = applyMetadataPatchInTransaction(library, transaction, targets.trackIds(), patch);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (reply.changes.empty())
        {
          return Unchanged<UpdateTrackMetadataReply>{.value = std::move(reply)};
        }

        auto mutatedIds =
          reply.changes | std::views::transform(&TrackChangeRecord::trackId) | std::ranges::to<std::vector>();
        return Changed<UpdateTrackMetadataReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      },
      "Update track metadata");

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);

    if (!executionRes->optCommittedRevision)
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, *executionRes->optCommittedRevision));
    return result;
  }

  Result<EditTrackTagsReply> LibraryWriter::Impl::previewEditTags(std::span<TrackId const> trackIds,
                                                                  std::span<std::string const> tagsToAdd,
                                                                  std::span<std::string const> tagsToRemove)
  {
    return applyInteractivePreview(
      mutationService,
      [this, trackIds, tagsToAdd, tagsToRemove](library::LibraryWrite& transaction)
      { return applyTagPatchInTransaction(library, transaction, trackIds, tagsToAdd, tagsToRemove); });
  }

  Result<LibraryWriter::TagAuthoringResult> LibraryWriter::Impl::applyEditTags(
    BoundTrackTargets const& targets,
    std::span<std::string const> tagsToAdd,
    std::span<std::string const> tagsToRemove)
  {
    auto start = mutationService.beginAuthoringMutation(targets);
    auto result = TagAuthoringResult{.status = start.status};

    if (!start.optMutation)
    {
      return result;
    }

    auto executionRes = start.optMutation->execute(
      [this, &targets, tagsToAdd, tagsToRemove](
        library::LibraryWrite& transaction) -> Result<OperationOutcome<EditTrackTagsReply>>
      {
        auto replyRes = applyTagPatchInTransaction(library, transaction, targets.trackIds(), tagsToAdd, tagsToRemove);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (reply.changes.empty())
        {
          return Unchanged<EditTrackTagsReply>{.value = std::move(reply)};
        }

        auto mutatedIds =
          reply.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
        return Changed<EditTrackTagsReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      },
      "Edit track tags");

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);

    if (!executionRes->optCommittedRevision)
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, *executionRes->optCommittedRevision));
    return result;
  }

  Result<AddTracksToListReply> LibraryWriter::Impl::previewAddTracksToList(ListId const listId,
                                                                           std::span<TrackId const> const trackIds)
  {
    return applyInteractivePreview(
      mutationService,
      [this, listId, trackIds](library::LibraryWrite& transaction)
      { return applyAddTracksToListInTransaction(library, transaction, listId, trackIds); });
  }

  Result<LibraryWriter::AddToListAuthoringResult> LibraryWriter::Impl::applyAddTracksToList(
    ListId const listId,
    BoundTrackTargets const& targets)
  {
    auto start = mutationService.beginAuthoringMutation(targets);
    auto result = AddToListAuthoringResult{.status = start.status};

    if (!start.optMutation)
    {
      return result;
    }

    auto executionRes = start.optMutation->execute(
      [this, listId, &targets](library::LibraryWrite& transaction) -> Result<OperationOutcome<AddTracksToListReply>>
      {
        auto replyRes = applyAddTracksToListInTransaction(library, transaction, listId, targets.trackIds());

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (reply.tagEdit.changes.empty())
        {
          return Unchanged<AddTracksToListReply>{.value = std::move(reply)};
        }

        auto mutatedIds =
          reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
        return Changed<AddTracksToListReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      },
      "Add tracks to list");

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);

    if (!executionRes->optCommittedRevision)
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, *executionRes->optCommittedRevision));
    return result;
  }

  Result<RemoveTracksFromListReply> LibraryWriter::Impl::previewRemoveTracksFromList(
    ListId const listId,
    std::span<TrackId const> const trackIds)
  {
    auto workRes = applyInteractivePreview(
      mutationService,
      [this, listId, trackIds](library::LibraryWrite& transaction)
      { return applyRemoveTracksFromListInTransaction(library, transaction, listId, trackIds); });

    if (!workRes)
    {
      return std::unexpected{workRes.error()};
    }

    return std::move(workRes->reply);
  }

  Result<LibraryWriter::RemoveFromListAuthoringResult> LibraryWriter::Impl::applyRemoveTracksFromList(
    ListId const listId,
    BoundTrackTargets const& targets)
  {
    auto start = mutationService.beginAuthoringMutation(targets);
    auto result = RemoveFromListAuthoringResult{.status = start.status};

    if (!start.optMutation)
    {
      return result;
    }

    auto executionRes = start.optMutation->execute(
      [this, listId, &targets](
        library::LibraryWrite& transaction) -> Result<OperationOutcome<RemoveTracksFromListReply>>
      {
        auto workRes = applyRemoveTracksFromListInTransaction(library, transaction, listId, targets.trackIds());

        if (!workRes)
        {
          return std::unexpected{workRes.error()};
        }

        auto work = std::move(*workRes);
        auto mutatedIds = work.reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) |
                          std::ranges::to<std::vector>();

        if (auto const orderChanged = work.optOrderChange.has_value(); mutatedIds.empty() && !orderChanged)
        {
          return Unchanged<RemoveTracksFromListReply>{.value = std::move(work.reply)};
        }

        auto changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)};

        if (work.optOrderChange)
        {
          changeSet.listsUpserted.push_back(listId);
          changeSet.listOrderChanges.push_back(std::move(*work.optOrderChange));
        }

        return Changed<RemoveTracksFromListReply>{
          .value = std::move(work.reply),
          .changeSet = std::move(changeSet),
        };
      },
      "Remove tracks from list");

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);

    if (!executionRes->optCommittedRevision)
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, *executionRes->optCommittedRevision));
    return result;
  }

  Result<ListId> LibraryWriter::Impl::createList(ListDraft const& draft)
  {
    auto executionRes =
      executeInteractiveMutation(mutationService,
                                 "Create list",
                                 [&draft](library::LibraryWrite& transaction) -> Result<OperationOutcome<ListId>>
                                 {
                                   auto listIdRes = createListInTransaction(transaction, draft);

                                   if (!listIdRes)
                                   {
                                     return std::unexpected{listIdRes.error()};
                                   }

                                   auto const listId = *listIdRes;
                                   return Changed<ListId>{
                                     .value = listId,
                                     .changeSet = LibraryChangeSet{.listsUpserted = {listId}},
                                   };
                                 });

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    AO_INVARIANT(executionRes->optCommittedRevision, "List creation did not commit its generated identity");
    return executionRes->value;
  }

  Result<> LibraryWriter::Impl::previewCreateList(ListDraft const& draft)
  {
    auto listIdRes = applyInteractivePreview(mutationService,
                                             [&draft](library::LibraryWrite& transaction)
                                             { return createListInTransaction(transaction, draft); });

    if (!listIdRes)
    {
      return std::unexpected{listIdRes.error()};
    }

    return {};
  }

  Result<UpdateListReply> LibraryWriter::Impl::updateList(ListDraft const& draft)
  {
    auto executionRes = executeInteractiveMutation(
      mutationService,
      "Update list",
      [&draft](library::LibraryWrite& transaction) -> Result<OperationOutcome<UpdateListReply>>
      {
        auto replyRes = updateListInTransaction(transaction, draft);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (!reply.changed)
        {
          return Unchanged<UpdateListReply>{.value = std::move(reply)};
        }

        return Changed<UpdateListReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {draft.listId}},
        };
      });

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    return std::move(executionRes->value);
  }

  Result<UpdateListReply> LibraryWriter::Impl::previewUpdateList(ListDraft const& draft)
  {
    return applyInteractivePreview(mutationService,
                                   [&draft](library::LibraryWrite& transaction)
                                   { return updateListInTransaction(transaction, draft); });
  }

  Result<LibraryWriter::MoveOrderAuthoringResult> LibraryWriter::Impl::applyMoveListOrder(
    BoundListOrder const& order,
    std::span<TrackId const> const selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    auto start = mutationService.beginListOrderAuthoringMutation(order);
    auto result = MoveOrderAuthoringResult{.status = start.status};

    if (!start.optMutation)
    {
      return result;
    }

    auto const effectiveTrackIds = order.effectiveTrackIds();
    auto const effectiveMembership = std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
    auto selectedMembership = std::unordered_set<TrackId>{};

    for (auto const trackId : selectedTrackIds)
    {
      if (!effectiveMembership.contains(trackId))
      {
        return makeError(
          Error::Code::InvalidInput, std::format("List order selection is not in the bound source: {}", trackId));
      }

      selectedMembership.insert(trackId);
    }

    for (auto const trackId : effectiveTrackIds)
    {
      if (selectedMembership.contains(trackId))
      {
        result.reply.selectedTrackIds.push_back(trackId);
      }
    }

    result.reply.optBeforeTrackId = optBeforeTrackId;

    if (result.reply.selectedTrackIds.empty())
    {
      result.status = ListOrderAuthoringStatus::NoOp;
      return result;
    }

    if (optBeforeTrackId &&
        (!effectiveMembership.contains(*optBeforeTrackId) || selectedMembership.contains(*optBeforeTrackId)))
    {
      return makeError(Error::Code::InvalidInput, "List order anchor must be an unselected bound source track");
    }

    auto desiredEffectiveTrackIds = std::vector<TrackId>{};
    desiredEffectiveTrackIds.reserve(effectiveTrackIds.size());

    for (auto const trackId : effectiveTrackIds)
    {
      if (!selectedMembership.contains(trackId))
      {
        desiredEffectiveTrackIds.push_back(trackId);
      }
    }

    auto insertion = desiredEffectiveTrackIds.end();

    if (optBeforeTrackId)
    {
      insertion = std::ranges::find(desiredEffectiveTrackIds, *optBeforeTrackId);
    }

    desiredEffectiveTrackIds.insert(
      insertion, result.reply.selectedTrackIds.begin(), result.reply.selectedTrackIds.end());

    if (std::ranges::equal(effectiveTrackIds, desiredEffectiveTrackIds))
    {
      result.status = ListOrderAuthoringStatus::NoOp;
      return result;
    }

    auto executionRes = start.optMutation->execute(
      [&order, &desiredEffectiveTrackIds, &result, optBeforeTrackId](
        library::LibraryWrite& transaction) -> Result<OperationOutcome<MoveListOrderReply>>
      {
        auto changeSetRes = applyMoveListOrderInTransaction(
          transaction, order, result.reply.selectedTrackIds, desiredEffectiveTrackIds, optBeforeTrackId);

        if (!changeSetRes)
        {
          return std::unexpected{changeSetRes.error()};
        }

        return Changed<MoveListOrderReply>{
          .value = std::move(result.reply),
          .changeSet = std::move(*changeSetRes),
        };
      },
      "Move list order");

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);
    result.status = ListOrderAuthoringStatus::Applied;
    return result;
  }

  Result<LibraryWriter::ResetOrderAuthoringResult> LibraryWriter::Impl::applyResetListOrder(BoundListOrder const& order)
  {
    auto start = mutationService.beginListOrderAuthoringMutation(order);
    auto result = ResetOrderAuthoringResult{.status = start.status};

    if (!start.optMutation)
    {
      return result;
    }

    auto executionRes = start.optMutation->execute(
      [&order](library::LibraryWrite& transaction) -> Result<OperationOutcome<ResetListOrderReply>>
      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, order.listId());

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto const oldOrderTrackIds = orderTrackIdsFrom(*viewRes);
        auto reply = ResetListOrderReply{.forgottenPositionCount = oldOrderTrackIds.size()};

        if (oldOrderTrackIds.empty())
        {
          return Unchanged<ResetListOrderReply>{.value = std::move(reply)};
        }

        auto list = listWithOrder(*viewRes, {});

        if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
        {
          return storageError("Failed to reset List order", updateRes.error());
        }

        return Changed<ResetListOrderReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {order.listId()},
                                        .listOrderChanges =
                                          {
                                            ListOrderChange{.listId = order.listId(), .operation = ListOrderReset{}},
                                          }},
        };
      },
      "Reset list order");

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);

    if (!executionRes->optCommittedRevision)
    {
      result.status = ListOrderAuthoringStatus::NoOp;
      return result;
    }

    result.status = ListOrderAuthoringStatus::Applied;
    return result;
  }

  Result<LibraryWriter::ForgetHiddenOrderAuthoringResult> LibraryWriter::Impl::applyForgetHiddenListOrder(
    BoundListOrder const& order)
  {
    auto start = mutationService.beginListOrderAuthoringMutation(order);
    auto result = ForgetHiddenOrderAuthoringResult{.status = start.status};

    if (!start.optMutation)
    {
      return result;
    }

    auto executionRes = start.optMutation->execute(
      [&order](library::LibraryWrite& transaction) -> Result<OperationOutcome<ForgetHiddenListOrderReply>>
      {
        auto listWriter = transaction.lists();
        auto viewRes = requireList(listWriter, order.listId());

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto const oldOrderTrackIds = orderTrackIdsFrom(*viewRes);
        auto const effectiveTrackIds = order.effectiveTrackIds();
        auto const effectiveMembership =
          std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
        auto nextOrderTrackIds = std::vector<TrackId>{};
        nextOrderTrackIds.reserve(oldOrderTrackIds.size());

        for (auto const trackId : oldOrderTrackIds)
        {
          if (effectiveMembership.contains(trackId))
          {
            nextOrderTrackIds.push_back(trackId);
          }
        }

        auto reply =
          ForgetHiddenListOrderReply{.forgottenPositionCount = oldOrderTrackIds.size() - nextOrderTrackIds.size()};

        if (reply.forgottenPositionCount == 0)
        {
          return Unchanged<ForgetHiddenListOrderReply>{.value = std::move(reply)};
        }

        auto list = listWithOrder(*viewRes, nextOrderTrackIds);

        if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
        {
          return storageError("Failed to forget hidden List positions", updateRes.error());
        }

        auto script = delta::diff(oldOrderTrackIds, nextOrderTrackIds);
        return Changed<ForgetHiddenListOrderReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {order.listId()},
                                        .listOrderChanges =
                                          {
                                            ListOrderChange{.listId = order.listId(), .operation = std::move(script)},
                                          }},
        };
      },
      "Forget hidden list order");

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    result.reply = std::move(executionRes->value);

    if (!executionRes->optCommittedRevision)
    {
      result.status = ListOrderAuthoringStatus::NoOp;
      return result;
    }

    result.status = ListOrderAuthoringStatus::Applied;
    return result;
  }

  Result<DeleteListReply> LibraryWriter::Impl::deleteList(ListId const listId, DeleteListOptions const options)
  {
    auto executionRes = executeInteractiveMutation(
      mutationService,
      "Delete list",
      [this, listId, options](library::LibraryWrite& transaction) -> Result<OperationOutcome<DeleteListReply>>
      {
        auto workRes = applyDeleteListInTransaction(library, transaction, listId, options);

        if (!workRes)
        {
          return std::unexpected{workRes.error()};
        }

        auto work = std::move(*workRes);
        return Changed<DeleteListReply>{
          .value = std::move(work.reply),
          .changeSet = std::move(work.changeSet),
        };
      });

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    return std::move(executionRes->value);
  }

  Result<DeleteListReply> LibraryWriter::Impl::previewDeleteList(ListId const listId, DeleteListOptions const options)
  {
    auto workRes =
      applyInteractivePreview(mutationService,
                              [this, listId, options](library::LibraryWrite& transaction)
                              { return applyDeleteListInTransaction(library, transaction, listId, options); });

    if (!workRes)
    {
      return std::unexpected{workRes.error()};
    }

    return std::move(workRes->reply);
  }

  Result<DeleteListSubtreeReply> LibraryWriter::Impl::deleteListAndDescendants(ListId const listId,
                                                                               DeleteListOptions const options)
  {
    auto executionRes = executeInteractiveMutation(
      mutationService,
      "Delete list subtree",
      [this, listId, options](library::LibraryWrite& transaction) -> Result<OperationOutcome<DeleteListSubtreeReply>>
      {
        auto workRes = applyDeleteListSubtreeInTransaction(library, transaction, listId, options);

        if (!workRes)
        {
          return std::unexpected{workRes.error()};
        }

        auto work = std::move(*workRes);
        return Changed<DeleteListSubtreeReply>{
          .value = std::move(work.reply),
          .changeSet = std::move(work.changeSet),
        };
      });

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    return std::move(executionRes->value);
  }

  Result<DeleteListSubtreeReply> LibraryWriter::Impl::previewDeleteListAndDescendants(ListId const listId,
                                                                                      DeleteListOptions const options)
  {
    auto workRes =
      applyInteractivePreview(mutationService,
                              [this, listId, options](library::LibraryWrite& transaction)
                              { return applyDeleteListSubtreeInTransaction(library, transaction, listId, options); });

    if (!workRes)
    {
      return std::unexpected{workRes.error()};
    }

    return std::move(workRes->reply);
  }

  Result<DeleteTrackReply> LibraryWriter::Impl::deleteTrack(TrackId const trackId)
  {
    auto executionRes = executeInteractiveMutation(
      mutationService,
      "Delete track",
      [this, trackId](library::LibraryWrite& transaction) -> Result<OperationOutcome<DeleteTrackReply>>
      {
        auto workRes = applyDeleteTrackInTransaction(library, transaction, trackId);

        if (!workRes)
        {
          return std::unexpected{workRes.error()};
        }

        auto work = std::move(*workRes);
        return Changed<DeleteTrackReply>{
          .value = std::move(work.reply),
          .changeSet = std::move(work.changeSet),
        };
      });

    if (!executionRes)
    {
      return std::unexpected{executionRes.error()};
    }

    return std::move(executionRes->value);
  }

  Result<DeleteTrackReply> LibraryWriter::Impl::previewDeleteTrack(TrackId const trackId)
  {
    auto workRes = applyInteractivePreview(mutationService,
                                           [this, trackId](library::LibraryWrite& transaction)
                                           { return applyDeleteTrackInTransaction(library, transaction, trackId); });

    if (!workRes)
    {
      return std::unexpected{workRes.error()};
    }

    return std::move(workRes->reply);
  }

  Result<CreateTrackReply> LibraryWriter::Impl::createTrackFromFile(std::filesystem::path const& path)
  {
    auto const targetRes = importTargetForPath(library, path);

    if (!targetRes)
    {
      return std::unexpected{targetRes.error()};
    }

    auto const& target = *targetRes;
    auto mediaTrackRes = readMediaTrack(target.fullPath);

    if (!mediaTrackRes)
    {
      return std::unexpected{mediaTrackRes.error()};
    }

    auto executionRes = executeInteractiveMutation(
      mutationService,
      "Create track",
      [&target, &mediaTrackRes](library::LibraryWrite& transaction) -> Result<OperationOutcome<CreateTrackFacts>>
      {
        auto factsRes = applyCreateTrackInTransaction(transaction, target, *mediaTrackRes);

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
      return std::unexpected{executionRes.error()};
    }

    AO_INVARIANT(executionRes->optCommittedRevision, "Track creation did not commit its generated identity");
    return committedCreateTrackReply(std::move(executionRes->value));
  }

  Result<PreviewCreateTrackReply> LibraryWriter::Impl::previewCreateTrackFromFile(std::filesystem::path const& path)
  {
    auto const targetRes = importTargetForPath(library, path);

    if (!targetRes)
    {
      return std::unexpected{targetRes.error()};
    }

    auto const& target = *targetRes;
    auto mediaTrackRes = readMediaTrack(target.fullPath);

    if (!mediaTrackRes)
    {
      return std::unexpected{mediaTrackRes.error()};
    }

    auto factsRes =
      applyInteractivePreview(mutationService,
                              [&target, &mediaTrackRes](library::LibraryWrite& transaction)
                              { return applyCreateTrackInTransaction(transaction, target, *mediaTrackRes); });

    if (!factsRes)
    {
      return std::unexpected{factsRes.error()};
    }

    return previewCreateTrackReply(std::move(*factsRes));
  }
} // namespace ao::rt
