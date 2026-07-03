// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

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
#include <ao/lmdb/Transaction.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/tag/TagFile.h>

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

    void applyTextMetadataPatch(library::TrackBuilder::MetadataBuilder& meta,
                                MetadataPatch const& patch,
                                PatchResult& result,
                                std::vector<TrackFieldChange>& changes)
    {
      applyStringPatch(
        patch.optTitle,
        "title",
        meta.title(),
        [&meta](std::string_view value) { meta.title(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optArtist,
        "artist",
        meta.artist(),
        [&meta](std::string_view value) { meta.artist(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optAlbum,
        "album",
        meta.album(),
        [&meta](std::string_view value) { meta.album(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optAlbumArtist,
        "albumArtist",
        meta.albumArtist(),
        [&meta](std::string_view value) { meta.albumArtist(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optGenre,
        "genre",
        meta.genre(),
        [&meta](std::string_view value) { meta.genre(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optComposer,
        "composer",
        meta.composer(),
        [&meta](std::string_view value) { meta.composer(value); },
        result.changedHot,
        changes);
      applyStringPatch(
        patch.optWork,
        "work",
        meta.work(),
        [&meta](std::string_view value) { meta.work(value); },
        result.changedCold,
        changes);
      applyStringPatch(
        patch.optMovement,
        "movement",
        meta.movement(),
        [&meta](std::string_view value) { meta.movement(value); },
        result.changedCold,
        changes);
    }

    void applyNumberMetadataPatch(library::TrackBuilder::MetadataBuilder& meta,
                                  MetadataPatch const& patch,
                                  PatchResult& result,
                                  std::vector<TrackFieldChange>& changes)
    {
      applyUint16Patch(
        patch.optYear,
        "year",
        meta.year(),
        [&meta](std::uint16_t value) { meta.year(value); },
        result.changedHot,
        changes);
      applyUint16Patch(
        patch.optMovementNumber,
        "movementNumber",
        meta.movementNumber(),
        [&meta](std::uint16_t value) { meta.movementNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optMovementTotal,
        "movementTotal",
        meta.movementTotal(),
        [&meta](std::uint16_t value) { meta.movementTotal(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optTrackNumber,
        "trackNumber",
        meta.trackNumber(),
        [&meta](std::uint16_t value) { meta.trackNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optTrackTotal,
        "trackTotal",
        meta.trackTotal(),
        [&meta](std::uint16_t value) { meta.trackTotal(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optDiscNumber,
        "discNumber",
        meta.discNumber(),
        [&meta](std::uint16_t value) { meta.discNumber(value); },
        result.changedCold,
        changes);
      applyUint16Patch(
        patch.optDiscTotal,
        "discTotal",
        meta.discTotal(),
        [&meta](std::uint16_t value) { meta.discTotal(value); },
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
      auto& meta = builder.metadata();
      auto result = PatchResult{};

      applyTextMetadataPatch(meta, patch, result, changes);
      applyNumberMetadataPatch(meta, patch, result, changes);
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

    Result<> validateSmartExpression(library::MusicLibrary& library, std::string const& expression)
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

      auto plan = query::compileQuery(*expr, &library.dictionary());

      if (!plan)
      {
        return prefixError("invalid list filter", plan.error());
      }

      return {};
    }

    Result<> validateListDraft(library::MusicLibrary& library,
                               library::ListStore::Writer const& listWriter,
                               LibraryWriter::ListDraft const& draft)
    {
      if (draft.kind == LibraryWriter::ListKind::Smart)
      {
        if (auto result = validateSmartExpression(library, draft.expression); !result)
        {
          return result;
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

    Result<std::vector<ListId>> removeTrackFromManualLists(library::MusicLibrary& library,
                                                           lmdb::WriteTransaction& txn,
                                                           TrackId trackId)
    {
      auto updates = std::vector<std::pair<ListId, std::vector<std::byte>>>{};

      {
        auto listReader = library.lists().reader(txn);

        for (auto const& [listId, view] : listReader)
        {
          if (view.isSmart() || !std::ranges::contains(view.tracks(), trackId))
          {
            continue;
          }

          auto builder = library::ListBuilder::fromView(view);
          builder.tracks().remove(trackId);
          updates.emplace_back(listId, builder.serialize());
        }
      }

      auto listWriter = library.lists().writer(txn);
      auto changed = std::vector<ListId>{};
      changed.reserve(updates.size());

      for (auto const& [listId, payload] : updates)
      {
        if (auto result = listWriter.update(listId, payload); !result)
        {
          return storageError("Failed to update list membership", result.error());
        }

        changed.push_back(listId);
      }

      return changed;
    }

    std::vector<std::byte> payloadForDraft(LibraryWriter::ListDraft const& draft)
    {
      auto builder =
        library::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

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

      return builder.serialize();
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

    std::vector<TrackId> trackIdsFrom(library::ListView::TrackProxy const& tracks)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(tracks.size());
      result.append_range(tracks);
      return result;
    }

    UpdateListReply diffListUpdate(library::ListView const& existing, LibraryWriter::ListDraft const& draft)
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

      auto const oldTrackIds = trackIdsFrom(existing.tracks());

      for (auto const trackId : draft.trackIds)
      {
        if (!std::ranges::contains(oldTrackIds, trackId) && !std::ranges::contains(reply.addedTrackIds, trackId))
        {
          reply.addedTrackIds.push_back(trackId);
        }
      }

      for (auto const trackId : oldTrackIds)
      {
        if (!std::ranges::contains(draft.trackIds, trackId) && !std::ranges::contains(reply.removedTrackIds, trackId))
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
  } // namespace

  struct LibraryWriter::Impl final
  {
    Result<UpdateTrackMetadataReply> applyUpdateMetadata(std::span<TrackId const> trackIds,
                                                         MetadataPatch const& patch,
                                                         MutationMode mode);
    Result<EditTrackTagsReply> applyEditTags(std::span<TrackId const> trackIds,
                                             std::span<std::string const> tagsToAdd,
                                             std::span<std::string const> tagsToRemove,
                                             MutationMode mode);
    Result<ListId> applyCreateList(ListDraft const& draft, MutationMode mode);
    Result<UpdateListReply> applyUpdateList(ListDraft const& draft, MutationMode mode);
    Result<DeleteListReply> applyDeleteList(ListId listId, MutationMode mode);
    Result<DeleteTrackReply> applyDeleteTrack(TrackId trackId, MutationMode mode);
    Result<CreateTrackReply> applyCreateTrackFromFile(std::filesystem::path const& path, MutationMode mode);

    library::MusicLibrary& library;
    LibraryChanges& changes;
  };

  LibraryWriter::LibraryWriter(library::MusicLibrary& library, LibraryChanges& changes)
    : _implPtr{std::make_unique<Impl>(library, changes)}
  {
  }

  LibraryWriter::~LibraryWriter() = default;

  Result<UpdateTrackMetadataReply> LibraryWriter::updateMetadata(std::span<TrackId const> trackIds,
                                                                 MetadataPatch const& patch)
  {
    return _implPtr->applyUpdateMetadata(trackIds, patch, MutationMode::Commit);
  }

  Result<UpdateTrackMetadataReply> LibraryWriter::previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                                        MetadataPatch const& patch)
  {
    return _implPtr->applyUpdateMetadata(trackIds, patch, MutationMode::Preview);
  }

  Result<EditTrackTagsReply> LibraryWriter::editTags(std::span<TrackId const> trackIds,
                                                     std::span<std::string const> tagsToAdd,
                                                     std::span<std::string const> tagsToRemove)
  {
    return _implPtr->applyEditTags(trackIds, tagsToAdd, tagsToRemove, MutationMode::Commit);
  }

  Result<EditTrackTagsReply> LibraryWriter::previewEditTags(std::span<TrackId const> trackIds,
                                                            std::span<std::string const> tagsToAdd,
                                                            std::span<std::string const> tagsToRemove)
  {
    return _implPtr->applyEditTags(trackIds, tagsToAdd, tagsToRemove, MutationMode::Preview);
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

  Result<UpdateTrackMetadataReply> LibraryWriter::Impl::applyUpdateMetadata(std::span<TrackId const> trackIds,
                                                                            MetadataPatch const& patch,
                                                                            MutationMode mode)
  {
    auto txn = library.writeTransaction();
    auto writer = library.tracks().writer(txn);
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
        auto hotDataResult = builder.serializeHot(txn, library.dictionary());

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
        auto coldDataResult = builder.serializeCold(txn, library.dictionary(), library.resources());

        if (!coldDataResult)
        {
          return storageError("Failed to serialize cold track data", coldDataResult.error());
        }

        if (auto result =
              writer.updateCold(trackId,
                                coldDataResult->size(),
                                [&](std::span<std::byte> buf) { std::ranges::copy(*coldDataResult, buf.begin()); });
            !result)
        {
          return storageError("Failed to update cold track data", result.error());
        }
      }

      mutated.push_back(trackId);
      changes.push_back(TrackChangeRecord{.trackId = trackId, .fields = std::move(fieldChanges)});
    }

    auto reply = UpdateTrackMetadataReply{.mutatedIds = std::move(mutated), .changes = std::move(changes)};

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit metadata update", result.error());
    }

    if (!reply.mutatedIds.empty())
    {
      this->changes.notifyTracksMutated(reply.mutatedIds);
    }

    return reply;
  }

  Result<EditTrackTagsReply> LibraryWriter::Impl::applyEditTags(std::span<TrackId const> trackIds,
                                                                std::span<std::string const> tagsToAdd,
                                                                std::span<std::string const> tagsToRemove,
                                                                MutationMode mode)
  {
    auto txn = library.writeTransaction();
    auto writer = library.tracks().writer(txn);
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

      auto hotDataResult = builder.serializeHot(txn, library.dictionary());

      if (!hotDataResult)
      {
        return storageError("Failed to serialize hot track data", hotDataResult.error());
      }

      if (auto result = writer.updateHot(trackId, *hotDataResult); !result)
      {
        return storageError("Failed to update hot track data", result.error());
      }

      mutated.push_back(trackId);
      changes.push_back(
        TrackTagsChange{.trackId = trackId, .addedTags = std::move(addedTags), .removedTags = std::move(removedTags)});
    }

    auto reply = EditTrackTagsReply{.mutatedIds = std::move(mutated), .changes = std::move(changes)};

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit tag update", result.error());
    }

    if (!reply.mutatedIds.empty())
    {
      this->changes.notifyTracksMutated(reply.mutatedIds);
    }

    return reply;
  }

  Result<ListId> LibraryWriter::Impl::applyCreateList(ListDraft const& draft, MutationMode mode)
  {
    auto txn = library.writeTransaction();
    auto listWriter = library.lists().writer(txn);

    if (auto result = validateListDraft(library, listWriter, draft); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const payload = payloadForDraft(draft);

    auto createResult = listWriter.create(payload);

    if (!createResult)
    {
      return storageError("Failed to create list", createResult.error());
    }

    auto const listId = createResult->first;

    if (mode == MutationMode::Preview)
    {
      return listId;
    }

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit list creation", result.error());
    }

    this->changes.notifyListsMutated({listId}, {});

    return listId;
  }

  Result<UpdateListReply> LibraryWriter::Impl::applyUpdateList(ListDraft const& draft, MutationMode mode)
  {
    auto txn = library.writeTransaction();
    auto listWriter = library.lists().writer(txn);
    auto optExisting = listWriter.get(draft.listId);

    if (!optExisting)
    {
      return makeError(Error::Code::NotFound, std::format("list not found: {}", draft.listId));
    }

    if (auto result = validateListDraft(library, listWriter, draft); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const payload = payloadForDraft(draft);
    auto reply = diffListUpdate(*optExisting, draft);

    if (std::ranges::equal(optExisting->rawData(), payload))
    {
      return reply;
    }

    reply.changed = true;

    if (auto result = listWriter.update(draft.listId, payload); !result)
    {
      return storageError("Failed to update list", result.error());
    }

    if (mode == MutationMode::Preview)
    {
      return reply;
    }

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit list update", result.error());
    }

    this->changes.notifyListsMutated({draft.listId}, {});
    return reply;
  }

  Result<DeleteListReply> LibraryWriter::Impl::applyDeleteList(ListId listId, MutationMode mode)
  {
    auto txn = library.writeTransaction();
    auto listWriter = library.lists().writer(txn);
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

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit list delete", result.error());
    }

    this->changes.notifyListsMutated({}, {listId});
    return reply;
  }

  Result<DeleteTrackReply> LibraryWriter::Impl::applyDeleteTrack(TrackId trackId, MutationMode mode)
  {
    auto txn = library.writeTransaction();

    auto writer = library.tracks().writer(txn);
    auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
    }

    auto const uri = std::string{optView->property().uri()};
    auto const title = std::string{optView->metadata().title()};
    auto changedListsResult = removeTrackFromManualLists(library, txn, trackId);

    if (!changedListsResult)
    {
      return std::unexpected{changedListsResult.error()};
    }

    auto changedLists = std::move(*changedListsResult);
    auto reply = DeleteTrackReply{.trackId = trackId, .uri = uri, .title = title, .removedFromListIds = changedLists};

    if (!uri.empty())
    {
      if (auto result = library.manifest().writer(txn).remove(uri); !result)
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

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit track delete", result.error());
    }

    if (!reply.removedFromListIds.empty())
    {
      this->changes.notifyListsMutated(reply.removedFromListIds, {});
    }

    this->changes.notifyTrackCollectionChanged({}, {trackId});
    this->changes.notifyTracksMutated({trackId});
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
    auto tagFileResult = tag::TagFile::open(target.fullPath);

    if (!tagFileResult)
    {
      return std::unexpected{tagFileResult.error()};
    }

    auto trackResult = (*tagFileResult)->loadTrack();

    if (!trackResult)
    {
      return std::unexpected{trackResult.error()};
    }

    auto txn = library.writeTransaction();
    auto writer = library.tracks().writer(txn);
    auto manifestWriter = library.manifest().writer(txn);

    auto existingManifest = manifestWriter.get(target.uri);

    if (existingManifest)
    {
      return makeError(Error::Code::Conflict, std::format("track file is already imported: {}", target.uri));
    }

    if (existingManifest.error().code != Error::Code::NotFound)
    {
      return storageError("Failed to read file manifest", existingManifest.error());
    }

    auto builder = *trackResult;
    builder.property().uri(target.uri);
    auto const title = std::string{builder.metadata().title()};
    auto const artist = std::string{builder.metadata().artist()};

    auto preparedResult = builder.prepare(txn, library.dictionary(), library.resources());

    if (!preparedResult)
    {
      return storageError("Failed to prepare track data", preparedResult.error());
    }

    auto& [preparedHot, preparedCold] = *preparedResult;
    auto createResult = library::createPreparedTrackData(writer, preparedHot, preparedCold);

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

    auto manifestBuilder = library::FileManifestBuilder::createNew();
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

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit track creation", result.error());
    }

    this->changes.notifyTrackCollectionChanged({id}, {});
    this->changes.notifyTracksMutated({id});
    return reply;
  }
} // namespace ao::rt
