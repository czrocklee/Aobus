// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryWriter.h>

#include "LibraryMutationService.h"
#include "MediaTrack.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    enum class MutationMode : std::uint8_t
    {
      Commit,
      Preview,
    };

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

    struct PreparedListPayload final
    {
      std::vector<std::byte> payload{};
    };

    Result<PreparedListPayload> payloadForDraft(LibraryWriter::ListDraft const& draft,
                                                std::optional<library::ListView> const& optExisting = std::nullopt)
    {
      auto builder = optExisting ? library::ListBuilder::fromView(*optExisting) : library::ListBuilder::makeEmpty();
      builder.name(draft.name).description(draft.description).filter(draft.expression).parentId(draft.parentId);
      auto payloadRes = builder.serialize();

      if (!payloadRes)
      {
        return std::unexpected{payloadRes.error()};
      }

      return PreparedListPayload{.payload = std::move(*payloadRes)};
    }

    Result<> validateListDraft(library::ListStore::Writer const& listWriter, LibraryWriter::ListDraft const& draft)
    {
      if (auto result = validateListExpression(draft.expression); !result)
      {
        return result;
      }

      if (draft.parentId == kInvalidListId)
      {
        return {};
      }

      if (draft.parentId == draft.listId)
      {
        return makeError(Error::Code::InvalidInput, "list parent cannot be the list itself");
      }

      auto seen = std::unordered_set<std::uint32_t>{};
      auto cursor = draft.parentId;

      while (cursor != kInvalidListId)
      {
        if (!seen.insert(cursor.raw()).second)
        {
          return makeError(Error::Code::InvalidInput, "list parent chain contains a cycle");
        }

        auto const optParent = listWriter.get(cursor);

        if (!optParent)
        {
          return makeError(Error::Code::InvalidInput, std::format("list parent not found: {}", cursor));
        }

        cursor = optParent->parentId();

        if (draft.listId != kInvalidListId && cursor == draft.listId)
        {
          return makeError(Error::Code::InvalidInput, "list parent cannot be a descendant of the list");
        }
      }

      return {};
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

    Result<std::vector<std::byte>> listPayloadWithOrder(library::ListView const& view,
                                                        std::span<TrackId const> orderTrackIds)
    {
      auto builder = library::ListBuilder::fromView(view);
      builder.orderTrackIds().clear();

      for (auto const trackId : orderTrackIds)
      {
        builder.orderTrackIds().add(trackId);
      }

      return builder.serialize();
    }

    Result<library::ListView> requireList(library::ListStore::Writer const& listWriter, ListId listId)
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
      std::vector<std::byte> payload{};
      ListOrderChange orderChange{};
    };

    Result<ListOrderRemovalResult> removeTrackFromListOrders(library::MusicLibrary& library,
                                                             library::WriteTransaction& transaction,
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
          auto payloadRes = builder.serialize();

          if (!payloadRes)
          {
            return std::unexpected{payloadRes.error()};
          }

          updates.push_back(PendingListOrderRemoval{
            .listId = listId,
            .payload = std::move(*payloadRes),
            .orderChange =
              ListOrderChange{
                .listId = listId,
                .operation = removalScriptFor(storedTrackIds, selectedTrackIds),
              },
          });
        }
      }

      auto listWriter = library.lists().writer(transaction);
      auto result = ListOrderRemovalResult{};
      result.changedListIds.reserve(updates.size());
      result.orderChanges.reserve(updates.size());

      for (auto& update : updates)
      {
        if (auto updateRes = listWriter.update(update.listId, update.payload); !updateRes)
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

    PreviewCreateTrackReply previewReply(CreateTrackReply reply)
    {
      return PreviewCreateTrackReply{
        .uri = std::move(reply.uri), .title = std::move(reply.title), .artist = std::move(reply.artist)};
    }

    Result<UpdateTrackMetadataReply> applyMetadataPatchInTransaction(library::MusicLibrary& library,
                                                                     library::WriteTransaction& transaction,
                                                                     std::span<TrackId const> trackIds,
                                                                     MetadataPatch const& patch)
    {
      auto writer = library.tracks().writer(transaction);
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
          auto preparedRes = builder.prepare(transaction, library.resources());

          if (!preparedRes)
          {
            return storageError("Failed to serialize cold track data", preparedRes.error());
          }

          auto const& [preparedHot, preparedCold] = *preparedRes;
          updateRes = library::updatePreparedTrackRecord(writer, trackId, preparedHot, preparedCold);
        }
        else if (patchRes.changedHot)
        {
          auto preparedRes = builder.prepareHot(transaction);

          if (!preparedRes)
          {
            return storageError("Failed to serialize hot track data", preparedRes.error());
          }

          updateRes = library::updatePreparedHotTrackRecord(writer, trackId, *preparedRes);
        }
        else
        {
          auto preparedRes = builder.prepareCold(transaction, library.resources());

          if (!preparedRes)
          {
            return storageError("Failed to serialize cold track data", preparedRes.error());
          }

          updateRes = library::updatePreparedColdTrackRecord(writer, trackId, *preparedRes);
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
                                                          library::WriteTransaction& transaction,
                                                          std::span<TrackId const> trackIds,
                                                          std::span<std::string const> tagsToAdd,
                                                          std::span<std::string const> tagsToRemove)
    {
      auto writer = library.tracks().writer(transaction);
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

        auto preparedRes = builder.prepareHot(transaction);

        if (!preparedRes)
        {
          return storageError("Failed to serialize hot track data", preparedRes.error());
        }

        if (auto result = library::updatePreparedHotTrackRecord(writer, trackId, *preparedRes); !result)
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
      library::WriteTransaction& transaction,
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
        if (!view.isHotValid())
        {
          return makeError(
            Error::Code::CorruptData, std::format("track {} contains an invalid hot record", trackId.raw()));
        }

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
                                                                  library::WriteTransaction& transaction,
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
          return makeError(Error::Code::InvalidState,
                           std::format("List disappeared while previewing subtree deletion: {}", currentId));
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

    Result<std::vector<ParentFilterPlan>> compileParentFilterPlans(library::MusicLibrary& library,
                                                                   library::WriteTransaction& transaction,
                                                                   library::ListView const& targetView)
    {
      auto plans = std::vector<ParentFilterPlan>{};
      auto reader = library.lists().reader(transaction);
      auto parentId = targetView.parentId();
      auto visited = std::unordered_set<ListId>{};

      while (parentId != kInvalidListId)
      {
        if (!visited.insert(parentId).second)
        {
          return makeError(Error::Code::InvalidState, "List parent cycle detected while validating membership");
        }

        auto optParent = reader.get(parentId);

        if (!optParent)
        {
          return makeError(
            Error::Code::InvalidState, std::format("List parent is missing while validating membership: {}", parentId));
        }

        if (!optParent->filter().empty())
        {
          auto expressionRes = query::parse(optParent->filter());

          if (!expressionRes)
          {
            return makeError(
              Error::Code::InvalidState, std::format("Stored parent List expression is invalid: {}", parentId));
          }

          auto planRes = query::compileQuery(*expressionRes);

          if (!planRes)
          {
            return makeError(
              Error::Code::InvalidState, std::format("Stored parent List expression cannot be compiled: {}", parentId));
          }

          plans.push_back(ParentFilterPlan{.listId = parentId, .plan = std::move(*planRes)});
        }

        parentId = optParent->parentId();
      }

      return plans;
    }

    Result<> validateTracksExist(library::MusicLibrary& library,
                                 library::WriteTransaction& transaction,
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
                                      library::WriteTransaction& transaction,
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
          if (!query::hasRequiredTrackData(plans[index].plan.accessProfile, *optTrack))
          {
            return makeError(
              Error::Code::CorruptData,
              std::format("Track {} contains invalid data required by parent List {}", trackId, plans[index].listId));
          }

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
                                                                   library::WriteTransaction& transaction,
                                                                   ListId const listId,
                                                                   std::span<TrackId const> trackIds)
    {
      auto listName = std::string{};
      auto tag = std::string{};

      // LMDB values are borrowed from the transaction and may be invalidated by
      // a later write. Materialize every List field used by the reply before
      // applyTagPatchInTransaction performs any database mutation.
      {
        auto listWriter = library.lists().writer(transaction);
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
                                                                            library::WriteTransaction& transaction,
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
      auto payload = std::vector<std::byte>{};

      {
        auto listWriter = library.lists().writer(transaction);
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
          auto payloadRes = listPayloadWithOrder(*viewRes, nextOrder);

          if (!payloadRes)
          {
            return std::unexpected{payloadRes.error()};
          }

          payload = std::move(*payloadRes);
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

      if (auto updateRes = library.lists().writer(transaction).update(listId, payload); !updateRes)
      {
        return storageError("Failed to forget removed List positions", updateRes.error());
      }

      work.optOrderChange = ListOrderChange{
        .listId = listId,
        .operation = removalScriptFor(oldOrder, selected),
      };
      return work;
    }

    Result<LibraryChangeSet> applyMoveListOrderInTransaction(library::MusicLibrary& library,
                                                             library::WriteTransaction& transaction,
                                                             BoundListOrder const& order,
                                                             std::span<TrackId const> selectedTrackIds,
                                                             std::span<TrackId const> desiredEffectiveTrackIds,
                                                             std::optional<TrackId> const optBeforeTrackId)
    {
      auto listWriter = library.lists().writer(transaction);
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

        if (orderInsertion == nextOrderTrackIds.end())
        {
          return makeError(Error::Code::InvalidState, "Bound List order anchor is absent from materialized order");
        }
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

      if (!std::ranges::equal(projectedOrder, desiredEffectiveTrackIds))
      {
        return makeError(Error::Code::InvalidState, "Materialized List order does not represent the requested move");
      }

      auto payloadRes = listPayloadWithOrder(view, nextOrderTrackIds);

      if (!payloadRes)
      {
        return std::unexpected{payloadRes.error()};
      }

      if (auto updateRes = listWriter.update(order.listId(), *payloadRes); !updateRes)
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
                                                        library::WriteTransaction& transaction,
                                                        ListId const listId,
                                                        DeleteListOptions const options)
    {
      auto listWriter = library.lists().writer(transaction);
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

      if (!listWriter.remove(listId))
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
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
    Result<ListId> applyCreateList(ListDraft const& draft, MutationMode mode);
    Result<UpdateListReply> applyUpdateList(ListDraft const& draft, MutationMode mode);
    Result<MoveOrderAuthoringResult> applyMoveListOrder(BoundListOrder const& order,
                                                        std::span<TrackId const> selectedTrackIds,
                                                        std::optional<TrackId> optBeforeTrackId);
    Result<ResetOrderAuthoringResult> applyResetListOrder(BoundListOrder const& order);
    Result<ForgetHiddenOrderAuthoringResult> applyForgetHiddenListOrder(BoundListOrder const& order);
    Result<DeleteListReply> applyDeleteList(ListId listId, DeleteListOptions options, MutationMode mode);
    Result<DeleteListSubtreeReply> applyDeleteListAndDescendants(ListId listId,
                                                                 DeleteListOptions options,
                                                                 MutationMode mode);
    Result<DeleteTrackReply> applyDeleteTrack(TrackId trackId, MutationMode mode);
    Result<CreateTrackReply> applyCreateTrackFromFile(std::filesystem::path const& path, MutationMode mode);

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
    return _implPtr->applyCreateList(draft, MutationMode::Commit);
  }

  Result<> LibraryWriter::previewCreateList(ListDraft const& draft)
  {
    auto result = _implPtr->applyCreateList(draft, MutationMode::Preview);

    if (!result)
    {
      return std::unexpected{result.error()};
    }

    return {};
  }

  Result<UpdateListReply> LibraryWriter::updateList(ListDraft const& draft)
  {
    return _implPtr->applyUpdateList(draft, MutationMode::Commit);
  }

  Result<UpdateListReply> LibraryWriter::previewUpdateList(ListDraft const& draft)
  {
    return _implPtr->applyUpdateList(draft, MutationMode::Preview);
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
    return _implPtr->applyDeleteList(listId, options, MutationMode::Commit);
  }

  Result<DeleteListReply> LibraryWriter::previewDeleteList(ListId listId, DeleteListOptions const options)
  {
    return _implPtr->applyDeleteList(listId, options, MutationMode::Preview);
  }

  Result<DeleteListSubtreeReply> LibraryWriter::deleteListAndDescendants(ListId const listId,
                                                                         DeleteListOptions const options)
  {
    return _implPtr->applyDeleteListAndDescendants(listId, options, MutationMode::Commit);
  }

  Result<DeleteListSubtreeReply> LibraryWriter::previewDeleteListAndDescendants(ListId const listId,
                                                                                DeleteListOptions const options)
  {
    return _implPtr->applyDeleteListAndDescendants(listId, options, MutationMode::Preview);
  }

  Result<DeleteTrackReply> LibraryWriter::deleteTrack(TrackId trackId)
  {
    return _implPtr->applyDeleteTrack(trackId, MutationMode::Commit);
  }

  Result<DeleteTrackReply> LibraryWriter::previewDeleteTrack(TrackId trackId)
  {
    return _implPtr->applyDeleteTrack(trackId, MutationMode::Preview);
  }

  Result<CreateTrackReply> LibraryWriter::createTrackFromFile(std::filesystem::path const& path)
  {
    return _implPtr->applyCreateTrackFromFile(path, MutationMode::Commit);
  }

  Result<PreviewCreateTrackReply> LibraryWriter::previewCreateTrackFromFile(std::filesystem::path const& path)
  {
    auto result = _implPtr->applyCreateTrackFromFile(path, MutationMode::Preview);

    if (!result)
    {
      return std::unexpected{result.error()};
    }

    return previewReply(std::move(*result));
  }

  Result<UpdateTrackMetadataReply> LibraryWriter::Impl::previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                                              MetadataPatch const& patch)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto replyRes = mutation.apply([this, trackIds, &patch](library::WriteTransaction& transaction)
                                   { return applyMetadataPatchInTransaction(library, transaction, trackIds, patch); });

    if (!replyRes)
    {
      return std::unexpected{replyRes.error()};
    }

    return std::move(*replyRes);
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

    auto replyRes = start.optMutation->apply(
      [this, &targets, &patch](library::WriteTransaction& transaction)
      { return applyMetadataPatchInTransaction(library, transaction, targets.trackIds(), patch); });

    if (!replyRes)
    {
      return std::unexpected{replyRes.error()};
    }

    result.reply = std::move(*replyRes);

    if (result.reply.changes.empty())
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    auto mutatedIds =
      result.reply.changes | std::views::transform(&TrackChangeRecord::trackId) | std::ranges::to<std::vector>();
    auto commitRes = start.optMutation->commit(LibraryChangeSet{.tracksMutated = std::move(mutatedIds)});

    if (!commitRes)
    {
      return storageError("Failed to commit metadata update", commitRes.error());
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, commitRes->libraryRevision));
    return result;
  }

  Result<EditTrackTagsReply> LibraryWriter::Impl::previewEditTags(std::span<TrackId const> trackIds,
                                                                  std::span<std::string const> tagsToAdd,
                                                                  std::span<std::string const> tagsToRemove)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto replyRes =
      mutation.apply([this, trackIds, tagsToAdd, tagsToRemove](library::WriteTransaction& transaction)
                     { return applyTagPatchInTransaction(library, transaction, trackIds, tagsToAdd, tagsToRemove); });

    if (!replyRes)
    {
      return std::unexpected{replyRes.error()};
    }

    return std::move(*replyRes);
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

    auto replyRes = start.optMutation->apply(
      [this, &targets, tagsToAdd, tagsToRemove](library::WriteTransaction& transaction)
      { return applyTagPatchInTransaction(library, transaction, targets.trackIds(), tagsToAdd, tagsToRemove); });

    if (!replyRes)
    {
      return std::unexpected{replyRes.error()};
    }

    result.reply = std::move(*replyRes);

    if (result.reply.changes.empty())
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    auto mutatedIds =
      result.reply.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
    auto commitRes = start.optMutation->commit(LibraryChangeSet{.tracksMutated = std::move(mutatedIds)});

    if (!commitRes)
    {
      return storageError("Failed to commit tag update", commitRes.error());
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, commitRes->libraryRevision));
    return result;
  }

  Result<AddTracksToListReply> LibraryWriter::Impl::previewAddTracksToList(ListId const listId,
                                                                           std::span<TrackId const> const trackIds)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    return mutation.apply([this, listId, trackIds](library::WriteTransaction& transaction)
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

    auto replyRes = start.optMutation->apply(
      [this, listId, &targets](library::WriteTransaction& transaction)
      { return applyAddTracksToListInTransaction(library, transaction, listId, targets.trackIds()); });

    if (!replyRes)
    {
      return std::unexpected{replyRes.error()};
    }

    result.reply = std::move(*replyRes);

    if (result.reply.tagEdit.changes.empty())
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    auto mutatedIds =
      result.reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
    auto commitRes = start.optMutation->commit(LibraryChangeSet{.tracksMutated = std::move(mutatedIds)});

    if (!commitRes)
    {
      return storageError("Failed to commit Add to List", commitRes.error());
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, commitRes->libraryRevision));
    return result;
  }

  Result<RemoveTracksFromListReply> LibraryWriter::Impl::previewRemoveTracksFromList(
    ListId const listId,
    std::span<TrackId const> const trackIds)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto workRes =
      mutation.apply([this, listId, trackIds](library::WriteTransaction& transaction)
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

    auto workRes = start.optMutation->apply(
      [this, listId, &targets](library::WriteTransaction& transaction)
      { return applyRemoveTracksFromListInTransaction(library, transaction, listId, targets.trackIds()); });

    if (!workRes)
    {
      return std::unexpected{workRes.error()};
    }

    auto work = std::move(*workRes);
    result.reply = std::move(work.reply);
    auto mutatedIds =
      result.reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();

    if (auto const orderChanged = work.optOrderChange.has_value(); mutatedIds.empty() && !orderChanged)
    {
      result.status = TrackAuthoringStatus::NoOp;
      return result;
    }

    auto changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)};

    if (work.optOrderChange)
    {
      changeSet.listsUpserted.push_back(listId);
      changeSet.listOrderChanges.push_back(std::move(*work.optOrderChange));
    }

    auto commitRes = start.optMutation->commit(std::move(changeSet));

    if (!commitRes)
    {
      return storageError("Failed to commit Remove from List", commitRes.error());
    }

    result.status = TrackAuthoringStatus::Applied;
    result.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, commitRes->libraryRevision));
    return result;
  }

  Result<ListId> LibraryWriter::Impl::applyCreateList(ListDraft const& draft, MutationMode mode)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto createRes = mutation.apply(
      [this, &draft](library::WriteTransaction& transaction) -> Result<ListId>
      {
        auto listWriter = library.lists().writer(transaction);
        auto preparedRes = payloadForDraft(draft);

        if (!preparedRes)
        {
          return std::unexpected{preparedRes.error()};
        }

        if (auto result = validateListDraft(listWriter, draft); !result)
        {
          return std::unexpected{result.error()};
        }

        auto result = listWriter.create(preparedRes->payload);

        if (!result)
        {
          return storageError("Failed to create list", result.error());
        }

        return *result;
      });

    if (!createRes)
    {
      return std::unexpected{createRes.error()};
    }

    auto const listId = *createRes;

    if (mode == MutationMode::Preview)
    {
      return listId;
    }

    if (auto result = mutation.commit(LibraryChangeSet{.listsUpserted = {listId}}); !result)
    {
      return storageError("Failed to commit list creation", result.error());
    }

    return listId;
  }

  Result<UpdateListReply> LibraryWriter::Impl::applyUpdateList(ListDraft const& draft, MutationMode mode)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto replyRes = mutation.apply(
      [this, &draft](library::WriteTransaction& transaction) -> Result<UpdateListReply>
      {
        auto listWriter = library.lists().writer(transaction);
        auto optExisting = listWriter.get(draft.listId);

        if (!optExisting)
        {
          return makeError(Error::Code::NotFound, std::format("list not found: {}", draft.listId));
        }

        auto preparedRes = payloadForDraft(draft, optExisting);

        if (!preparedRes)
        {
          return std::unexpected{preparedRes.error()};
        }

        if (auto result = validateListDraft(listWriter, draft); !result)
        {
          return std::unexpected{result.error()};
        }

        auto reply = diffListUpdate(*optExisting, draft);

        if (std::ranges::equal(optExisting->rawData(), preparedRes->payload))
        {
          return reply;
        }

        reply.changed = true;

        if (auto result = listWriter.update(draft.listId, preparedRes->payload); !result)
        {
          return storageError("Failed to update list", result.error());
        }

        return reply;
      });

    if (!replyRes)
    {
      return std::unexpected{replyRes.error()};
    }

    auto reply = std::move(*replyRes);

    if (!reply.changed || mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(LibraryChangeSet{.listsUpserted = {draft.listId}}); !result)
    {
      return storageError("Failed to commit list update", result.error());
    }

    return reply;
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

    auto changeSetRes = start.optMutation->apply(
      [this, &order, &desiredEffectiveTrackIds, &result, optBeforeTrackId](library::WriteTransaction& transaction)
      {
        return applyMoveListOrderInTransaction(
          library, transaction, order, result.reply.selectedTrackIds, desiredEffectiveTrackIds, optBeforeTrackId);
      });

    if (!changeSetRes)
    {
      return std::unexpected{changeSetRes.error()};
    }

    auto commitRes = start.optMutation->commit(std::move(*changeSetRes));

    if (!commitRes)
    {
      return storageError("Failed to commit List order move", commitRes.error());
    }

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

    auto changedRes = start.optMutation->apply(
      [this, &order, &result](library::WriteTransaction& transaction) -> Result<bool>
      {
        auto listWriter = library.lists().writer(transaction);
        auto viewRes = requireList(listWriter, order.listId());

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto const oldOrderTrackIds = orderTrackIdsFrom(*viewRes);
        result.reply.forgottenPositionCount = oldOrderTrackIds.size();

        if (oldOrderTrackIds.empty())
        {
          return false;
        }

        auto payloadRes = listPayloadWithOrder(*viewRes, {});

        if (!payloadRes)
        {
          return std::unexpected{payloadRes.error()};
        }

        if (auto updateRes = listWriter.update(order.listId(), *payloadRes); !updateRes)
        {
          return storageError("Failed to reset List order", updateRes.error());
        }

        return true;
      });

    if (!changedRes)
    {
      return std::unexpected{changedRes.error()};
    }

    if (!*changedRes)
    {
      result.status = ListOrderAuthoringStatus::NoOp;
      return result;
    }

    auto commitRes = start.optMutation->commit(
      LibraryChangeSet{.listsUpserted = {order.listId()},
                       .listOrderChanges = {
                         ListOrderChange{.listId = order.listId(), .operation = ListOrderReset{}},
                       }});

    if (!commitRes)
    {
      return storageError("Failed to commit List order reset", commitRes.error());
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

    auto changeSetRes = start.optMutation->apply(
      [this, &order, &result](library::WriteTransaction& transaction) -> Result<std::optional<LibraryChangeSet>>
      {
        auto listWriter = library.lists().writer(transaction);
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

        result.reply.forgottenPositionCount = oldOrderTrackIds.size() - nextOrderTrackIds.size();

        if (result.reply.forgottenPositionCount == 0)
        {
          return std::optional<LibraryChangeSet>{};
        }

        auto payloadRes = listPayloadWithOrder(*viewRes, nextOrderTrackIds);

        if (!payloadRes)
        {
          return std::unexpected{payloadRes.error()};
        }

        if (auto updateRes = listWriter.update(order.listId(), *payloadRes); !updateRes)
        {
          return storageError("Failed to forget hidden List positions", updateRes.error());
        }

        auto script = delta::diff(oldOrderTrackIds, nextOrderTrackIds);
        return std::optional{
          LibraryChangeSet{.listsUpserted = {order.listId()},
                           .listOrderChanges = {
                             ListOrderChange{.listId = order.listId(), .operation = std::move(script)},
                           }}};
      });

    if (!changeSetRes)
    {
      return std::unexpected{changeSetRes.error()};
    }

    if (!*changeSetRes)
    {
      result.status = ListOrderAuthoringStatus::NoOp;
      return result;
    }

    auto commitRes = start.optMutation->commit(std::move(**changeSetRes));

    if (!commitRes)
    {
      return storageError("Failed to commit hidden List position cleanup", commitRes.error());
    }

    result.status = ListOrderAuthoringStatus::Applied;
    return result;
  }

  Result<DeleteListReply> LibraryWriter::Impl::applyDeleteList(ListId listId,
                                                               DeleteListOptions const options,
                                                               MutationMode mode)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto workRes = mutation.apply([this, listId, options](library::WriteTransaction& transaction)
                                  { return applyDeleteListInTransaction(library, transaction, listId, options); });

    if (!workRes)
    {
      return std::unexpected{workRes.error()};
    }

    auto work = std::move(*workRes);

    if (mode == MutationMode::Preview)
    {
      return std::move(work.reply);
    }

    if (auto result = mutation.commit(std::move(work.changeSet)); !result)
    {
      return storageError("Failed to commit list delete", result.error());
    }

    return std::move(work.reply);
  }

  Result<DeleteListSubtreeReply> LibraryWriter::Impl::applyDeleteListAndDescendants(ListId const listId,
                                                                                    DeleteListOptions const options,
                                                                                    MutationMode const mode)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto reply = DeleteListSubtreeReply{};
    auto changeSet = LibraryChangeSet{};
    auto applyRes = mutation.apply(
      [this, listId, options, &reply, &changeSet](library::WriteTransaction& transaction) -> Result<>
      {
        auto deletedListsRes = collectDeleteListSubtree(library, transaction, listId);

        if (!deletedListsRes)
        {
          return std::unexpected{deletedListsRes.error()};
        }

        reply = DeleteListSubtreeReply{.rootListId = listId, .deletedLists = std::move(*deletedListsRes)};
        auto deletedIds =
          reply.deletedLists | std::views::transform(&DeleteListReply::listId) | std::ranges::to<std::vector>();
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

        auto listWriter = library.lists().writer(transaction);

        for (auto const& deleted : std::views::reverse(reply.deletedLists))
        {
          if (!listWriter.remove(deleted.listId))
          {
            return makeError(
              Error::Code::NotFound, std::format("list not found while deleting subtree: {}", deleted.listId));
          }
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
          reply.deletedLists.front().optTagImpact = optTagImpactWork->impact;
        }

        changeSet =
          LibraryChangeSet{.tracksMutated = std::move(mutatedTrackIds), .listsDeleted = std::move(deletedIds)};
        return {};
      });

    if (!applyRes)
    {
      return std::unexpected{applyRes.error()};
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(std::move(changeSet)); !result)
    {
      return storageError("Failed to commit List subtree delete", result.error());
    }

    return reply;
  }

  Result<DeleteTrackReply> LibraryWriter::Impl::applyDeleteTrack(TrackId trackId, MutationMode mode)
  {
    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto reply = DeleteTrackReply{};
    auto changeSet = LibraryChangeSet{};
    auto applyRes = mutation.apply(
      [this, trackId, &reply, &changeSet](library::WriteTransaction& transaction) -> Result<>
      {
        auto writer = library.tracks().writer(transaction);
        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optView)
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }

        if (!optView->isHotValid() || !optView->isColdValid())
        {
          return makeError(Error::Code::CorruptData, std::format("track {} contains an invalid record", trackId.raw()));
        }

        auto const uri = std::string{optView->property().uri()};
        auto const title = std::string{optView->metadata().title()};
        auto changedListsRes = removeTrackFromListOrders(library, transaction, trackId);

        if (!changedListsRes)
        {
          return std::unexpected{changedListsRes.error()};
        }

        auto changedLists = std::move(*changedListsRes);
        reply = DeleteTrackReply{
          .trackId = trackId,
          .uri = uri,
          .title = title,
          .removedFromListIds = changedLists.changedListIds,
        };

        if (!uri.empty())
        {
          if (auto result = library.manifest().writer(transaction).remove(uri); !result)
          {
            return storageError("Failed to remove file manifest entry", result.error());
          }
        }

        if (!writer.remove(trackId))
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }

        changeSet = LibraryChangeSet{.tracksDeleted = {trackId},
                                     .listsUpserted = reply.removedFromListIds,
                                     .listOrderChanges = std::move(changedLists.orderChanges)};
        return {};
      });

    if (!applyRes)
    {
      return std::unexpected{applyRes.error()};
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(std::move(changeSet)); !result)
    {
      return storageError("Failed to commit track delete", result.error());
    }

    return reply;
  }

  Result<CreateTrackReply> LibraryWriter::Impl::applyCreateTrackFromFile(std::filesystem::path const& path,
                                                                         MutationMode mode)
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

    auto mutationRes = mutationService.beginInteractiveMutation();

    if (!mutationRes)
    {
      return std::unexpected{mutationRes.error()};
    }

    auto mutation = std::move(*mutationRes);
    auto reply = CreateTrackReply{};
    auto changeSet = LibraryChangeSet{};
    auto applyRes = mutation.apply(
      [this, &target, &mediaTrackRes, &reply, &changeSet](library::WriteTransaction& transaction) -> Result<>
      {
        auto writer = library.tracks().writer(transaction);
        auto manifestWriter = library.manifest().writer(transaction);

        auto existingManifestRes = manifestWriter.get(target.uri);

        if (existingManifestRes)
        {
          return makeError(Error::Code::Conflict, std::format("track file is already imported: {}", target.uri));
        }

        if (existingManifestRes.error().code != Error::Code::NotFound)
        {
          return storageError("Failed to read file manifest", existingManifestRes.error());
        }

        auto& builder = mediaTrackRes->builder();
        builder.property().uri(target.uri);
        auto const title = std::string{builder.metadata().title()};
        auto const artist = std::string{builder.metadata().artist()};

        auto preparedRes = builder.prepare(transaction, library.resources());

        if (!preparedRes)
        {
          return storageError("Failed to prepare track data", preparedRes.error());
        }

        auto& [preparedHot, preparedCold] = *preparedRes;
        auto createRes = library::createPreparedTrackRecord(writer, preparedHot, preparedCold);

        if (!createRes)
        {
          return storageError("Failed to create track data", createRes.error());
        }

        auto const id = *createRes;

        auto fileEc = std::error_code{};
        auto const fileSize = std::filesystem::file_size(target.fullPath, fileEc);

        if (fileEc)
        {
          return makeError(
            Error::Code::IoError,
            std::format(
              "failed to inspect track file '{}': {}", utility::pathToUtf8(target.fullPath), fileEc.message()));
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
        manifestBuilder.trackId(id)
          .fileSize(static_cast<std::uint64_t>(fileSize))
          .mtime(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count()));

        if (auto putRes = manifestWriter.put(target.uri, manifestBuilder.serialize()); !putRes)
        {
          return storageError("Failed to update file manifest", putRes.error());
        }

        reply = CreateTrackReply{.trackId = id, .uri = target.uri, .title = title, .artist = artist};
        changeSet = LibraryChangeSet{.tracksInserted = {id}};
        return {};
      });

    if (!applyRes)
    {
      return std::unexpected{applyRes.error()};
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(std::move(changeSet)); !result)
    {
      return storageError("Failed to commit track creation", result.error());
    }

    return reply;
  }
} // namespace ao::rt
