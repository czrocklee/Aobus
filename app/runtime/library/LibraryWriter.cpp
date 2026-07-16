// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryMutationService.h"
#include "MediaTrack.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
          std::format("failed to resolve music root '{}': {}", library.rootPath().string(), ec.message()));
      }

      bool sawOutsideRoot = false;

      auto const resolveInsideRoot =
        [&root, &sawOutsideRoot](std::filesystem::path const& candidate) -> std::optional<std::filesystem::path>
      {
        auto ec = std::error_code{};
        auto fullPath = std::filesystem::weakly_canonical(candidate, ec);

        if (ec || !std::filesystem::is_regular_file(fullPath, ec) || ec)
        {
          return std::nullopt;
        }

        auto const rel = std::filesystem::relative(fullPath, root, ec);

        if (ec || rel.empty() || rel.is_absolute())
        {
          return std::nullopt;
        }

        for (auto const& part : rel)
        {
          if (part == "..")
          {
            sawOutsideRoot = true;
            return std::nullopt;
          }
        }

        return fullPath;
      };

      auto optFullPath = resolveInsideRoot(path);

      if (!optFullPath && !path.is_absolute())
      {
        optFullPath = resolveInsideRoot(root / path);
      }

      if (!optFullPath)
      {
        if (sawOutsideRoot)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("track file is outside music root: {}", path.string()));
        }

        return makeError(
          Error::Code::NotFound, std::format("track file not found under music root: {}", path.string()));
      }

      auto const rel = std::filesystem::relative(*optFullPath, root, ec);

      if (ec)
      {
        return makeError(Error::Code::IoError,
                         std::format("failed to resolve track URI for '{}': {}", optFullPath->string(), ec.message()));
      }

      return ImportTarget{.fullPath = std::move(*optFullPath), .uri = rel.generic_string()};
    }

    Result<> validateSmartExpression(std::string const& expression)
    {
      if (expression.empty())
      {
        return {};
      }

      auto expr = query::parse(expression);

      if (!expr)
      {
        return prefixError("invalid list filter", expr.error());
      }

      if (auto plan = query::compileQuery(*expr); !plan)
      {
        return prefixError("invalid list filter", plan.error());
      }

      return {};
    }

    struct PreparedListPayload final
    {
      std::vector<std::byte> payload{};
      std::vector<TrackId> canonicalTrackIds{};
    };

    PreparedListPayload payloadForDraft(LibraryWriter::ListDraft const& draft)
    {
      auto builder =
        library::ListBuilder::makeEmpty().name(draft.name).description(draft.description).parentId(draft.parentId);

      if (draft.kind == LibraryWriter::ListKind::Smart)
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

      auto canonicalTrackIds = std::vector<TrackId>{builder.tracks().ids().begin(), builder.tracks().ids().end()};
      return PreparedListPayload{.payload = builder.serialize(), .canonicalTrackIds = std::move(canonicalTrackIds)};
    }

    Result<> validateListDraft(library::ListStore::Writer const& listWriter,
                               library::TrackStore::Writer const& trackWriter,
                               LibraryWriter::ListDraft const& draft,
                               std::span<TrackId const> canonicalTrackIds)
    {
      if (draft.kind == LibraryWriter::ListKind::Smart)
      {
        if (auto result = validateSmartExpression(draft.expression); !result)
        {
          return result;
        }
      }
      else
      {
        for (auto const trackId : canonicalTrackIds)
        {
          if (trackId == kInvalidTrackId || !trackWriter.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
          {
            return makeError(Error::Code::InvalidInput, std::format("manual list track not found: {}", trackId));
          }
        }
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

    std::vector<TrackId> trackIdsFrom(library::ListView::TrackProxy const& tracks)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(tracks.size());
      result.append_range(tracks);
      return result;
    }

    std::vector<TrackId> canonicalTrackIdsFrom(library::ListView const& view)
    {
      if (view.isSmart())
      {
        return {};
      }

      auto builder = library::ListBuilder::fromView(view);
      return {builder.tracks().ids().begin(), builder.tracks().ids().end()};
    }

    std::vector<std::byte> manualListPayload(library::ListView const& view, std::span<TrackId const> trackIds)
    {
      auto builder = library::ListBuilder::fromView(view);
      builder.tracks().clear();

      for (auto const trackId : trackIds)
      {
        builder.tracks().add(trackId);
      }

      return builder.serialize();
    }

    Result<library::ListView> requireManualList(library::ListStore::Writer const& listWriter, ListId listId)
    {
      auto optView = listWriter.get(listId);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      if (optView->isSmart())
      {
        return makeError(Error::Code::InvalidInput, std::format("list is not manual: {}", listId));
      }

      return *optView;
    }

    std::vector<ManualStoredRemoveRange> removalRangesFor(std::span<TrackId const> storedTrackIds,
                                                          std::unordered_set<TrackId> const& selectedTrackIds)
    {
      auto removals = std::vector<ManualStoredRemoveRange>{};

      for (std::size_t index = 0; index < storedTrackIds.size(); ++index)
      {
        auto const trackId = storedTrackIds[index];

        if (!selectedTrackIds.contains(trackId))
        {
          continue;
        }

        if (removals.empty() || removals.back().start + removals.back().trackIds.size() != index)
        {
          removals.push_back(ManualStoredRemoveRange{.start = index});
        }

        removals.back().trackIds.push_back(trackId);
      }

      std::ranges::reverse(removals);
      return removals;
    }

    struct ManualListRemovalResult final
    {
      std::vector<ListId> changedListIds{};
      std::vector<ManualListContentChange> contentChanges{};
    };

    struct PendingManualListRemoval final
    {
      ListId listId = kInvalidListId;
      std::vector<std::byte> payload{};
      ManualListContentChange contentChange{};
    };

    Result<ManualListRemovalResult> removeTrackFromManualLists(library::MusicLibrary& library,
                                                               library::WriteTransaction& transaction,
                                                               TrackId trackId)
    {
      auto updates = std::vector<PendingManualListRemoval>{};

      {
        auto listReader = library.lists().reader(transaction);

        for (auto const& [listId, view] : listReader)
        {
          if (view.isSmart() || !std::ranges::contains(view.tracks(), trackId))
          {
            continue;
          }

          auto const storedTrackIds = trackIdsFrom(view.tracks());
          auto const selectedTrackIds = std::unordered_set<TrackId>{trackId};
          auto builder = library::ListBuilder::fromView(view);
          builder.tracks().remove(trackId);
          updates.push_back(PendingManualListRemoval{
            .listId = listId,
            .payload = builder.serialize(),
            .contentChange =
              ManualListContentChange{
                .listId = listId,
                .operation = ManualTracksRemove{.removals = removalRangesFor(storedTrackIds, selectedTrackIds)},
              },
          });
        }
      }

      auto listWriter = library.lists().writer(transaction);
      auto result = ManualListRemovalResult{};
      result.changedListIds.reserve(updates.size());
      result.contentChanges.reserve(updates.size());

      for (auto& update : updates)
      {
        if (auto updateResult = listWriter.update(update.listId, update.payload); !updateResult)
        {
          return storageError("Failed to update list membership", updateResult.error());
        }

        result.changedListIds.push_back(update.listId);
        result.contentChanges.push_back(std::move(update.contentChange));
      }

      return result;
    }

    std::string listKindName(bool isSmart)
    {
      return isSmart ? "smart" : "manual";
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

    UpdateListReply diffListUpdate(library::ListView const& existing,
                                   LibraryWriter::ListDraft const& draft,
                                   std::span<TrackId const> newTrackIds)
    {
      auto reply = UpdateListReply{};
      appendListFieldChange(reply.fieldChanges, "name", existing.name(), draft.name);
      appendListFieldChange(reply.fieldChanges, "description", existing.description(), draft.description);
      appendListFieldChange(reply.fieldChanges,
                            "type",
                            listKindName(existing.isSmart()),
                            listKindName(draft.kind == LibraryWriter::ListKind::Smart));
      appendListFieldChange(reply.fieldChanges,
                            "parentId",
                            std::format("{}", existing.parentId().raw()),
                            std::format("{}", draft.parentId.raw()));

      auto const newFilter =
        draft.kind == LibraryWriter::ListKind::Smart ? std::string_view{draft.expression} : std::string_view{};
      appendListFieldChange(reply.fieldChanges, "filter", existing.filter(), newFilter);

      auto const oldTrackIds = canonicalTrackIdsFrom(existing);
      reply.trackOrderChanged = !std::ranges::equal(oldTrackIds, newTrackIds);

      for (auto const trackId : newTrackIds)
      {
        if (!std::ranges::contains(oldTrackIds, trackId))
        {
          reply.addedTrackIds.push_back(trackId);
        }
      }

      for (auto const trackId : oldTrackIds)
      {
        if (!std::ranges::contains(newTrackIds, trackId))
        {
          reply.removedTrackIds.push_back(trackId);
        }
      }

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
      auto mutated = std::vector<TrackId>{};
      auto changes = std::vector<TrackChangeRecord>{};

      for (auto const trackId : trackIds)
      {
        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromView(*optView, library.dictionary());
        auto fieldChanges = std::vector<TrackFieldChange>{};
        auto const patchResult = applyMetadataPatch(builder, patch, fieldChanges);

        if (!patchResult.changedHot && !patchResult.changedCold)
        {
          continue;
        }

        if (patchResult.changedHot)
        {
          auto hotDataResult = builder.serializeHot(transaction);

          if (!hotDataResult)
          {
            return storageError("Failed to serialize hot track data", hotDataResult.error());
          }

          if (auto result = writer.updateHot(trackId, *hotDataResult); !result)
          {
            return storageError("Failed to update hot track data", result.error());
          }
        }

        if (patchResult.changedCold)
        {
          auto coldDataResult = builder.serializeCold(transaction, library.resources());

          if (!coldDataResult)
          {
            return storageError("Failed to serialize cold track data", coldDataResult.error());
          }

          if (auto result = writer.updateCold(trackId,
                                              coldDataResult->size(),
                                              [&](std::span<std::byte> buffer)
                                              { std::ranges::copy(*coldDataResult, buffer.begin()); });
              !result)
          {
            return storageError("Failed to update cold track data", result.error());
          }
        }

        mutated.push_back(trackId);
        changes.push_back(TrackChangeRecord{.trackId = trackId, .fields = std::move(fieldChanges)});
      }

      return UpdateTrackMetadataReply{.mutatedIds = std::move(mutated), .changes = std::move(changes)};
    }

    Result<EditTrackTagsReply> applyTagPatchInTransaction(library::MusicLibrary& library,
                                                          library::WriteTransaction& transaction,
                                                          std::span<TrackId const> trackIds,
                                                          std::span<std::string const> tagsToAdd,
                                                          std::span<std::string const> tagsToRemove)
    {
      auto writer = library.tracks().writer(transaction);
      auto mutated = std::vector<TrackId>{};
      auto changes = std::vector<TrackTagsChange>{};

      for (auto const trackId : trackIds)
      {
        auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

        if (!optView)
        {
          continue;
        }

        auto builder = library::TrackBuilder::fromView(*optView, library.dictionary());
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

        auto hotDataResult = builder.serializeHot(transaction);

        if (!hotDataResult)
        {
          return storageError("Failed to serialize hot track data", hotDataResult.error());
        }

        if (auto result = writer.updateHot(trackId, *hotDataResult); !result)
        {
          return storageError("Failed to update hot track data", result.error());
        }

        mutated.push_back(trackId);
        changes.push_back(TrackTagsChange{
          .trackId = trackId, .addedTags = std::move(addedTags), .removedTags = std::move(removedTags)});
      }

      return EditTrackTagsReply{.mutatedIds = std::move(mutated), .changes = std::move(changes)};
    }
  } // namespace

  struct LibraryWriter::Impl final
  {
    Result<UpdateTrackMetadataReply> previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                           MetadataPatch const& patch);
    Result<MetadataAuthoringOutcome> applyUpdateMetadata(BoundTrackTargets const& targets, MetadataPatch const& patch);
    Result<EditTrackTagsReply> previewEditTags(std::span<TrackId const> trackIds,
                                               std::span<std::string const> tagsToAdd,
                                               std::span<std::string const> tagsToRemove);
    Result<TagAuthoringOutcome> applyEditTags(BoundTrackTargets const& targets,
                                              std::span<std::string const> tagsToAdd,
                                              std::span<std::string const> tagsToRemove);
    Result<ListId> applyCreateList(ListDraft const& draft, MutationMode mode);
    Result<UpdateListReply> applyUpdateList(ListDraft const& draft, MutationMode mode);
    Result<InsertManualListTracksReply> applyInsertManualListTracks(ListId listId,
                                                                    std::size_t insertionIndex,
                                                                    std::span<TrackId const> trackIds,
                                                                    MutationMode mode);
    Result<RemoveManualListTracksReply> applyRemoveManualListTracks(ListId listId,
                                                                    std::span<TrackId const> trackIds,
                                                                    MutationMode mode);
    Result<MoveManualListTracksReply> applyMoveManualListTracks(ListId listId,
                                                                std::span<TrackId const> trackIds,
                                                                std::size_t insertionIndexAfterRemoval,
                                                                MutationMode mode);
    Result<DeleteListReply> applyDeleteList(ListId listId, MutationMode mode);
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

  Result<LibraryWriter::MetadataAuthoringOutcome> LibraryWriter::updateMetadata(BoundTrackTargets const& targets,
                                                                                MetadataPatch const& patch)
  {
    return _implPtr->applyUpdateMetadata(targets, patch);
  }

  Result<UpdateTrackMetadataReply> LibraryWriter::previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                                        MetadataPatch const& patch)
  {
    return _implPtr->previewUpdateMetadata(trackIds, patch);
  }

  Result<LibraryWriter::TagAuthoringOutcome> LibraryWriter::editTags(BoundTrackTargets const& targets,
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

  Result<InsertManualListTracksReply> LibraryWriter::insertManualListTracks(ListId listId,
                                                                            std::size_t insertionIndex,
                                                                            std::span<TrackId const> trackIds)
  {
    return _implPtr->applyInsertManualListTracks(listId, insertionIndex, trackIds, MutationMode::Commit);
  }

  Result<InsertManualListTracksReply> LibraryWriter::previewInsertManualListTracks(ListId listId,
                                                                                   std::size_t insertionIndex,
                                                                                   std::span<TrackId const> trackIds)
  {
    return _implPtr->applyInsertManualListTracks(listId, insertionIndex, trackIds, MutationMode::Preview);
  }

  Result<RemoveManualListTracksReply> LibraryWriter::removeManualListTracks(ListId listId,
                                                                            std::span<TrackId const> trackIds)
  {
    return _implPtr->applyRemoveManualListTracks(listId, trackIds, MutationMode::Commit);
  }

  Result<RemoveManualListTracksReply> LibraryWriter::previewRemoveManualListTracks(ListId listId,
                                                                                   std::span<TrackId const> trackIds)
  {
    return _implPtr->applyRemoveManualListTracks(listId, trackIds, MutationMode::Preview);
  }

  Result<MoveManualListTracksReply> LibraryWriter::moveManualListTracks(ListId listId,
                                                                        std::span<TrackId const> trackIds,
                                                                        std::size_t insertionIndexAfterRemoval)
  {
    return _implPtr->applyMoveManualListTracks(listId, trackIds, insertionIndexAfterRemoval, MutationMode::Commit);
  }

  Result<MoveManualListTracksReply> LibraryWriter::previewMoveManualListTracks(ListId listId,
                                                                               std::span<TrackId const> trackIds,
                                                                               std::size_t insertionIndexAfterRemoval)
  {
    return _implPtr->applyMoveManualListTracks(listId, trackIds, insertionIndexAfterRemoval, MutationMode::Preview);
  }

  Result<DeleteListReply> LibraryWriter::deleteList(ListId listId)
  {
    return _implPtr->applyDeleteList(listId, MutationMode::Commit);
  }

  Result<DeleteListReply> LibraryWriter::previewDeleteList(ListId listId)
  {
    return _implPtr->applyDeleteList(listId, MutationMode::Preview);
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
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto replyResult = applyMetadataPatchInTransaction(library, mutation.transaction(), trackIds, patch);

    if (!replyResult)
    {
      return std::unexpected{replyResult.error()};
    }

    return std::move(*replyResult);
  }

  Result<LibraryWriter::MetadataAuthoringOutcome> LibraryWriter::Impl::applyUpdateMetadata(
    BoundTrackTargets const& targets,
    MetadataPatch const& patch)
  {
    auto start = mutationService.beginAuthoringMutation(targets);
    auto outcome = MetadataAuthoringOutcome{
      .status = start.status,
      .missingTargetIds = std::move(start.missingTargetIds),
    };

    if (!start.optMutation)
    {
      return outcome;
    }

    auto replyResult =
      applyMetadataPatchInTransaction(library, start.optMutation->transaction(), targets.trackIds(), patch);

    if (!replyResult)
    {
      return std::unexpected{replyResult.error()};
    }

    outcome.reply = std::move(*replyResult);

    if (outcome.reply.mutatedIds.empty())
    {
      outcome.status = TrackAuthoringStatus::NoOp;
      return outcome;
    }

    auto commitResult = start.optMutation->commit(LibraryChangeSet{.tracksMutated = outcome.reply.mutatedIds});

    if (!commitResult)
    {
      return storageError("Failed to commit metadata update", commitResult.error());
    }

    outcome.status = TrackAuthoringStatus::Applied;
    outcome.libraryRevision = commitResult->libraryRevision;
    outcome.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, commitResult->libraryRevision));
    return outcome;
  }

  Result<EditTrackTagsReply> LibraryWriter::Impl::previewEditTags(std::span<TrackId const> trackIds,
                                                                  std::span<std::string const> tagsToAdd,
                                                                  std::span<std::string const> tagsToRemove)
  {
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto replyResult = applyTagPatchInTransaction(library, mutation.transaction(), trackIds, tagsToAdd, tagsToRemove);

    if (!replyResult)
    {
      return std::unexpected{replyResult.error()};
    }

    return std::move(*replyResult);
  }

  Result<LibraryWriter::TagAuthoringOutcome> LibraryWriter::Impl::applyEditTags(
    BoundTrackTargets const& targets,
    std::span<std::string const> tagsToAdd,
    std::span<std::string const> tagsToRemove)
  {
    auto start = mutationService.beginAuthoringMutation(targets);
    auto outcome = TagAuthoringOutcome{
      .status = start.status,
      .missingTargetIds = std::move(start.missingTargetIds),
    };

    if (!start.optMutation)
    {
      return outcome;
    }

    auto replyResult = applyTagPatchInTransaction(
      library, start.optMutation->transaction(), targets.trackIds(), tagsToAdd, tagsToRemove);

    if (!replyResult)
    {
      return std::unexpected{replyResult.error()};
    }

    outcome.reply = std::move(*replyResult);

    if (outcome.reply.mutatedIds.empty())
    {
      outcome.status = TrackAuthoringStatus::NoOp;
      return outcome;
    }

    auto commitResult = start.optMutation->commit(LibraryChangeSet{.tracksMutated = outcome.reply.mutatedIds});

    if (!commitResult)
    {
      return storageError("Failed to commit tag update", commitResult.error());
    }

    outcome.status = TrackAuthoringStatus::Applied;
    outcome.libraryRevision = commitResult->libraryRevision;
    outcome.optNextTargets.emplace(mutationService.advanceBoundTargets(targets, commitResult->libraryRevision));
    return outcome;
  }

  Result<ListId> LibraryWriter::Impl::applyCreateList(ListDraft const& draft, MutationMode mode)
  {
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();
    auto listWriter = library.lists().writer(transaction);
    auto trackWriter = library.tracks().writer(transaction);
    auto prepared = payloadForDraft(draft);

    if (auto result = validateListDraft(listWriter, trackWriter, draft, prepared.canonicalTrackIds); !result)
    {
      return std::unexpected{result.error()};
    }

    auto createResult = listWriter.create(prepared.payload);

    if (!createResult)
    {
      return storageError("Failed to create list", createResult.error());
    }

    auto const listId = createResult->first;

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
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();
    auto listWriter = library.lists().writer(transaction);
    auto trackWriter = library.tracks().writer(transaction);
    auto optExisting = listWriter.get(draft.listId);

    if (!optExisting)
    {
      return makeError(Error::Code::NotFound, std::format("list not found: {}", draft.listId));
    }

    auto const existingWasManual = !optExisting->isSmart();
    auto prepared = payloadForDraft(draft);

    if (auto result = validateListDraft(listWriter, trackWriter, draft, prepared.canonicalTrackIds); !result)
    {
      return std::unexpected{result.error()};
    }

    auto reply = diffListUpdate(*optExisting, draft, prepared.canonicalTrackIds);

    if (std::ranges::equal(optExisting->rawData(), prepared.payload))
    {
      return reply;
    }

    reply.changed = true;

    if (auto result = listWriter.update(draft.listId, prepared.payload); !result)
    {
      return storageError("Failed to update list", result.error());
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    auto manualContentChanges = std::vector<ManualListContentChange>{};

    if (existingWasManual && draft.kind == ListKind::Manual && reply.trackOrderChanged)
    {
      manualContentChanges.push_back(ManualListContentChange{.listId = draft.listId, .operation = ManualTracksReset{}});
    }

    if (auto result = mutation.commit(
          LibraryChangeSet{.listsUpserted = {draft.listId}, .manualContentChanges = std::move(manualContentChanges)});
        !result)
    {
      return storageError("Failed to commit list update", result.error());
    }

    return reply;
  }

  Result<InsertManualListTracksReply> LibraryWriter::Impl::applyInsertManualListTracks(
    ListId listId,
    std::size_t insertionIndex,
    std::span<TrackId const> trackIds,
    MutationMode mode)
  {
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();
    auto listWriter = library.lists().writer(transaction);
    auto viewResult = requireManualList(listWriter, listId);

    if (!viewResult)
    {
      return std::unexpected{viewResult.error()};
    }

    auto const& view = *viewResult;
    auto storedTrackIds = canonicalTrackIdsFrom(view);

    if (insertionIndex > storedTrackIds.size())
    {
      return makeError(
        Error::Code::InvalidInput,
        std::format("manual list insertion index {} exceeds stored size {}", insertionIndex, storedTrackIds.size()));
    }

    auto reply = InsertManualListTracksReply{.insertionIndex = insertionIndex};
    auto const storedMembership = std::unordered_set<TrackId>{storedTrackIds.begin(), storedTrackIds.end()};
    auto seenRequest = std::unordered_set<TrackId>{};
    auto trackWriter = library.tracks().writer(transaction);

    for (auto const trackId : trackIds)
    {
      if (!seenRequest.insert(trackId).second)
      {
        reply.duplicateRequest.push_back(trackId);
      }
      else if (trackId == kInvalidTrackId || !trackWriter.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
      {
        reply.missingTrack.push_back(trackId);
      }
      else if (storedMembership.contains(trackId))
      {
        reply.alreadyPresent.push_back(trackId);
      }
      else
      {
        reply.insertedTrackIds.push_back(trackId);
      }
    }

    if (reply.insertedTrackIds.empty())
    {
      return reply;
    }

    reply.changed = true;
    storedTrackIds.insert(storedTrackIds.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
                          reply.insertedTrackIds.begin(),
                          reply.insertedTrackIds.end());
    auto const payload = manualListPayload(view, storedTrackIds);

    if (auto result = listWriter.update(listId, payload); !result)
    {
      return storageError("Failed to insert manual list tracks", result.error());
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(LibraryChangeSet{
          .listsUpserted = {listId},
          .manualContentChanges = {ManualListContentChange{
            .listId = listId,
            .operation = ManualTracksInsert{.storedIndex = insertionIndex, .trackIds = reply.insertedTrackIds},
          }}});
        !result)
    {
      return storageError("Failed to commit manual list track insertion", result.error());
    }

    return reply;
  }

  Result<RemoveManualListTracksReply>
  LibraryWriter::Impl::applyRemoveManualListTracks(ListId listId, std::span<TrackId const> trackIds, MutationMode mode)
  {
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();
    auto listWriter = library.lists().writer(transaction);
    auto viewResult = requireManualList(listWriter, listId);

    if (!viewResult)
    {
      return std::unexpected{viewResult.error()};
    }

    auto const& view = *viewResult;
    auto storedTrackIds = canonicalTrackIdsFrom(view);
    auto const storedMembership = std::unordered_set<TrackId>{storedTrackIds.begin(), storedTrackIds.end()};
    auto seenRequest = std::unordered_set<TrackId>{};
    auto selectedTrackIds = std::unordered_set<TrackId>{};
    auto reply = RemoveManualListTracksReply{};

    for (auto const trackId : trackIds)
    {
      if (!seenRequest.insert(trackId).second)
      {
        reply.duplicateRequest.push_back(trackId);
      }
      else if (!storedMembership.contains(trackId))
      {
        reply.notPresent.push_back(trackId);
      }
      else
      {
        selectedTrackIds.insert(trackId);
      }
    }

    for (auto const trackId : storedTrackIds)
    {
      if (selectedTrackIds.contains(trackId))
      {
        reply.removedTrackIds.push_back(trackId);
      }
    }

    if (reply.removedTrackIds.empty())
    {
      return reply;
    }

    reply.changed = true;
    auto removals = removalRangesFor(storedTrackIds, selectedTrackIds);
    std::erase_if(storedTrackIds, [&selectedTrackIds](TrackId trackId) { return selectedTrackIds.contains(trackId); });
    auto const payload = manualListPayload(view, storedTrackIds);

    if (auto result = listWriter.update(listId, payload); !result)
    {
      return storageError("Failed to remove manual list tracks", result.error());
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result =
          mutation.commit(LibraryChangeSet{.listsUpserted = {listId},
                                           .manualContentChanges = {ManualListContentChange{
                                             .listId = listId,
                                             .operation = ManualTracksRemove{.removals = std::move(removals)},
                                           }}});
        !result)
    {
      return storageError("Failed to commit manual list track removal", result.error());
    }

    return reply;
  }

  Result<MoveManualListTracksReply> LibraryWriter::Impl::applyMoveManualListTracks(
    ListId listId,
    std::span<TrackId const> trackIds,
    std::size_t insertionIndexAfterRemoval,
    MutationMode mode)
  {
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();
    auto listWriter = library.lists().writer(transaction);
    auto viewResult = requireManualList(listWriter, listId);

    if (!viewResult)
    {
      return std::unexpected{viewResult.error()};
    }

    auto const& view = *viewResult;
    auto const storedTrackIds = canonicalTrackIdsFrom(view);
    auto const storedMembership = std::unordered_set<TrackId>{storedTrackIds.begin(), storedTrackIds.end()};
    auto seenRequest = std::unordered_set<TrackId>{};
    auto selectedMembership = std::unordered_set<TrackId>{};
    auto reply = MoveManualListTracksReply{.insertionIndexAfterRemoval = insertionIndexAfterRemoval};

    for (auto const trackId : trackIds)
    {
      if (!seenRequest.insert(trackId).second)
      {
        reply.duplicateRequest.push_back(trackId);
      }
      else if (!storedMembership.contains(trackId))
      {
        reply.notPresent.push_back(trackId);
      }
      else
      {
        selectedMembership.insert(trackId);
      }
    }

    auto remainingTrackIds = std::vector<TrackId>{};
    remainingTrackIds.reserve(storedTrackIds.size());

    for (auto const trackId : storedTrackIds)
    {
      if (selectedMembership.contains(trackId))
      {
        reply.selectedTrackIds.push_back(trackId);
      }
      else
      {
        remainingTrackIds.push_back(trackId);
      }
    }

    if (insertionIndexAfterRemoval > remainingTrackIds.size())
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("manual list move insertion index {} exceeds remaining size {}",
                                   insertionIndexAfterRemoval,
                                   remainingTrackIds.size()));
    }

    auto nextTrackIds = remainingTrackIds;
    nextTrackIds.insert(nextTrackIds.begin() + static_cast<std::ptrdiff_t>(insertionIndexAfterRemoval),
                        reply.selectedTrackIds.begin(),
                        reply.selectedTrackIds.end());

    if (nextTrackIds == storedTrackIds)
    {
      return reply;
    }

    reply.changed = true;
    auto removals = removalRangesFor(storedTrackIds, selectedMembership);
    auto const payload = manualListPayload(view, nextTrackIds);

    if (auto result = listWriter.update(listId, payload); !result)
    {
      return storageError("Failed to move manual list tracks", result.error());
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(
          LibraryChangeSet{.listsUpserted = {listId},
                           .manualContentChanges = {ManualListContentChange{
                             .listId = listId,
                             .operation = ManualTracksMove{.removals = std::move(removals),
                                                           .insertionIndexAfterRemoval = insertionIndexAfterRemoval,
                                                           .insertedTrackIds = reply.selectedTrackIds},
                           }}});
        !result)
    {
      return storageError("Failed to commit manual list track move", result.error());
    }

    return reply;
  }

  Result<DeleteListReply> LibraryWriter::Impl::applyDeleteList(ListId listId, MutationMode mode)
  {
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();
    auto listWriter = library.lists().writer(transaction);
    auto optView = listWriter.get(listId);

    if (!optView)
    {
      return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
    }

    auto reply = DeleteListReply{.listId = listId,
                                 .name = std::string{optView->name()},
                                 .kind = listKindName(optView->isSmart()),
                                 .trackCount = static_cast<std::uint64_t>(optView->tracks().size())};

    if (!listWriter.remove(listId))
    {
      return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(LibraryChangeSet{.listsDeleted = {listId}}); !result)
    {
      return storageError("Failed to commit list delete", result.error());
    }

    return reply;
  }

  Result<DeleteTrackReply> LibraryWriter::Impl::applyDeleteTrack(TrackId trackId, MutationMode mode)
  {
    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();

    auto writer = library.tracks().writer(transaction);
    auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
    }

    auto const uri = std::string{optView->property().uri()};
    auto const title = std::string{optView->metadata().title()};
    auto changedListsResult = removeTrackFromManualLists(library, transaction, trackId);

    if (!changedListsResult)
    {
      return std::unexpected{changedListsResult.error()};
    }

    auto changedLists = std::move(*changedListsResult);
    auto reply = DeleteTrackReply{
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

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(LibraryChangeSet{.tracksDeleted = {trackId},
                                                       .listsUpserted = reply.removedFromListIds,
                                                       .manualContentChanges = std::move(changedLists.contentChanges)});
        !result)
    {
      return storageError("Failed to commit track delete", result.error());
    }

    return reply;
  }

  Result<CreateTrackReply> LibraryWriter::Impl::applyCreateTrackFromFile(std::filesystem::path const& path,
                                                                         MutationMode mode)
  {
    auto const targetResult = importTargetForPath(library, path);

    if (!targetResult)
    {
      return std::unexpected{targetResult.error()};
    }

    auto const& target = *targetResult;
    auto mediaTrackResult = readMediaTrack(target.fullPath);

    if (!mediaTrackResult)
    {
      return std::unexpected{mediaTrackResult.error()};
    }

    auto mutationResult = mutationService.beginInteractiveMutation();

    if (!mutationResult)
    {
      return std::unexpected{mutationResult.error()};
    }

    auto mutation = std::move(*mutationResult);
    auto& transaction = mutation.transaction();
    auto writer = library.tracks().writer(transaction);
    auto manifestWriter = library.manifest().writer(transaction);

    auto existingManifest = manifestWriter.get(target.uri);

    if (existingManifest)
    {
      return makeError(Error::Code::Conflict, std::format("track file is already imported: {}", target.uri));
    }

    if (existingManifest.error().code != Error::Code::NotFound)
    {
      return storageError("Failed to read file manifest", existingManifest.error());
    }

    auto& builder = mediaTrackResult->builder();
    builder.property().uri(target.uri);
    auto const title = std::string{builder.metadata().title()};
    auto const artist = std::string{builder.metadata().artist()};

    auto preparedResult = builder.prepare(transaction, library.resources());

    if (!preparedResult)
    {
      return storageError("Failed to prepare track data", preparedResult.error());
    }

    auto& [preparedHot, preparedCold] = *preparedResult;
    auto createResult = library::createPreparedTrackRecord(writer, preparedHot, preparedCold);

    if (!createResult)
    {
      return storageError("Failed to create track data", createResult.error());
    }

    auto const [id, trackView] = *createResult;

    auto fileEc = std::error_code{};
    auto const fileSize = std::filesystem::file_size(target.fullPath, fileEc);

    if (fileEc)
    {
      return makeError(
        Error::Code::IoError,
        std::format("failed to inspect track file '{}': {}", target.fullPath.string(), fileEc.message()));
    }

    auto const lastWriteTime = std::filesystem::last_write_time(target.fullPath, fileEc);

    if (fileEc)
    {
      return makeError(
        Error::Code::IoError,
        std::format("failed to read track file timestamp '{}': {}", target.fullPath.string(), fileEc.message()));
    }

    auto manifestBuilder = library::FileManifestBuilder::makeEmpty();
    manifestBuilder.trackId(id)
      .fileSize(static_cast<std::uint64_t>(fileSize))
      .mtime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count()));

    if (auto putResult = manifestWriter.put(target.uri, manifestBuilder.serialize()); !putResult)
    {
      return storageError("Failed to update file manifest", putResult.error());
    }

    auto reply = CreateTrackReply{.trackId = id, .uri = target.uri, .title = title, .artist = artist};

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = mutation.commit(LibraryChangeSet{.tracksInserted = {id}}); !result)
    {
      return storageError("Failed to commit track creation", result.error());
    }

    return reply;
  }
} // namespace ao::rt
