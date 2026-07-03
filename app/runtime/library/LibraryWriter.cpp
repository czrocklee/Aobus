// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
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
    struct PatchResult final
    {
      bool changedHot = false;
      bool changedCold = false;
    };

    template<typename Setter>
    void applyStringPatch(std::optional<std::string> const& optValue,
                          std::string_view current,
                          Setter setter,
                          bool& changed)
    {
      if (!optValue || current == *optValue)
      {
        return;
      }

      setter(*optValue);
      changed = true;
    }

    template<typename Setter>
    void applyUint16Patch(std::optional<std::uint16_t> const& optValue,
                          std::uint16_t current,
                          Setter setter,
                          bool& changed)
    {
      if (!optValue || current == *optValue)
      {
        return;
      }

      setter(*optValue);
      changed = true;
    }

    void applyTextMetadataPatch(library::TrackBuilder::MetadataBuilder& meta,
                                MetadataPatch const& patch,
                                PatchResult& result)
    {
      applyStringPatch(
        patch.optTitle, meta.title(), [&meta](std::string_view value) { meta.title(value); }, result.changedHot);
      applyStringPatch(
        patch.optArtist, meta.artist(), [&meta](std::string_view value) { meta.artist(value); }, result.changedHot);
      applyStringPatch(
        patch.optAlbum, meta.album(), [&meta](std::string_view value) { meta.album(value); }, result.changedHot);
      applyStringPatch(
        patch.optAlbumArtist,
        meta.albumArtist(),
        [&meta](std::string_view value) { meta.albumArtist(value); },
        result.changedHot);
      applyStringPatch(
        patch.optGenre, meta.genre(), [&meta](std::string_view value) { meta.genre(value); }, result.changedHot);
      applyStringPatch(
        patch.optComposer,
        meta.composer(),
        [&meta](std::string_view value) { meta.composer(value); },
        result.changedHot);
      applyStringPatch(
        patch.optWork, meta.work(), [&meta](std::string_view value) { meta.work(value); }, result.changedCold);
      applyStringPatch(
        patch.optMovement,
        meta.movement(),
        [&meta](std::string_view value) { meta.movement(value); },
        result.changedCold);
    }

    void applyNumberMetadataPatch(library::TrackBuilder::MetadataBuilder& meta,
                                  MetadataPatch const& patch,
                                  PatchResult& result)
    {
      applyUint16Patch(
        patch.optYear, meta.year(), [&meta](std::uint16_t value) { meta.year(value); }, result.changedHot);
      applyUint16Patch(
        patch.optMovementNumber,
        meta.movementNumber(),
        [&meta](std::uint16_t value) { meta.movementNumber(value); },
        result.changedCold);
      applyUint16Patch(
        patch.optMovementTotal,
        meta.movementTotal(),
        [&meta](std::uint16_t value) { meta.movementTotal(value); },
        result.changedCold);
      applyUint16Patch(
        patch.optTrackNumber,
        meta.trackNumber(),
        [&meta](std::uint16_t value) { meta.trackNumber(value); },
        result.changedCold);
      applyUint16Patch(
        patch.optTrackTotal,
        meta.trackTotal(),
        [&meta](std::uint16_t value) { meta.trackTotal(value); },
        result.changedCold);
      applyUint16Patch(
        patch.optDiscNumber,
        meta.discNumber(),
        [&meta](std::uint16_t value) { meta.discNumber(value); },
        result.changedCold);
      applyUint16Patch(
        patch.optDiscTotal,
        meta.discTotal(),
        [&meta](std::uint16_t value) { meta.discTotal(value); },
        result.changedCold);
    }

    void applyCustomMetadataPatch(library::TrackBuilder& builder, MetadataPatch const& patch, PatchResult& result)
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

        builder.customMetadata().remove(key);

        if (optValue)
        {
          builder.customMetadata().add(key, *optValue);
        }

        result.changedCold = true;
      }
    }

    PatchResult applyMetadataPatch(library::TrackBuilder& builder, MetadataPatch const& patch)
    {
      auto& meta = builder.metadata();
      auto result = PatchResult{};

      applyTextMetadataPatch(meta, patch, result);
      applyNumberMetadataPatch(meta, patch, result);
      applyCustomMetadataPatch(builder, patch, result);

      return result;
    }

    void throwStorageError(char const* action, Error const& error)
    {
      auto const message = std::format("{}: {}", action, error.message);
      throwException<Exception>(std::string_view{message}, error.location);
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

    template<typename Transaction>
    void commitOrThrow(Transaction& txn, char const* action)
    {
      if (auto result = txn.commit(); !result)
      {
        throwStorageError(action, result.error());
      }
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

    std::vector<ListId> removeTrackFromManualLists(library::MusicLibrary& library,
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
          throwStorageError("Failed to update list membership", result.error());
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
  } // namespace

  struct LibraryWriter::Impl final
  {
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
    auto txn = _implPtr->library.writeTransaction();
    auto writer = _implPtr->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        continue;
      }

      auto builder = library::TrackBuilder::fromView(*optView, _implPtr->library.dictionary());
      auto const patchResult = applyMetadataPatch(builder, patch);

      if (!patchResult.changedHot && !patchResult.changedCold)
      {
        continue;
      }

      if (patchResult.changedHot)
      {
        auto hotDataResult = builder.serializeHot(txn, _implPtr->library.dictionary());

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
        auto coldDataResult = builder.serializeCold(txn, _implPtr->library.dictionary(), _implPtr->library.resources());

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
    }

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit metadata update", result.error());
    }

    if (!mutated.empty())
    {
      _implPtr->changes.notifyTracksMutated(mutated);
    }

    return UpdateTrackMetadataReply{.mutatedIds = std::move(mutated)};
  }

  Result<EditTrackTagsReply> LibraryWriter::editTags(std::span<TrackId const> trackIds,
                                                     std::span<std::string const> tagsToAdd,
                                                     std::span<std::string const> tagsToRemove)
  {
    auto txn = _implPtr->library.writeTransaction();
    auto writer = _implPtr->library.tracks().writer(txn);
    auto mutated = std::vector<TrackId>{};

    for (auto const trackId : trackIds)
    {
      auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Hot);

      if (!optView)
      {
        continue;
      }

      auto builder = library::TrackBuilder::fromView(*optView, _implPtr->library.dictionary());
      auto& tags = builder.tags();
      bool changed = false;

      for (auto const& tag : tagsToAdd)
      {
        if (!std::ranges::contains(tags.names(), tag))
        {
          tags.add(tag);
          changed = true;
        }
      }

      for (auto const& tag : tagsToRemove)
      {
        if (std::ranges::contains(tags.names(), tag))
        {
          tags.remove(tag);
          changed = true;
        }
      }

      if (!changed)
      {
        continue;
      }

      auto hotDataResult = builder.serializeHot(txn, _implPtr->library.dictionary());

      if (!hotDataResult)
      {
        return storageError("Failed to serialize hot track data", hotDataResult.error());
      }

      if (auto result = writer.updateHot(trackId, *hotDataResult); !result)
      {
        return storageError("Failed to update hot track data", result.error());
      }

      mutated.push_back(trackId);
    }

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit tag update", result.error());
    }

    if (!mutated.empty())
    {
      _implPtr->changes.notifyTracksMutated(mutated);
    }

    return EditTrackTagsReply{.mutatedIds = std::move(mutated)};
  }

  Result<ListId> LibraryWriter::createList(ListDraft const& draft)
  {
    auto txn = _implPtr->library.writeTransaction();
    auto listWriter = _implPtr->library.lists().writer(txn);

    if (auto result = validateListDraft(_implPtr->library, listWriter, draft); !result)
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

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit list creation", result.error());
    }

    _implPtr->changes.notifyListsMutated({listId}, {});

    return listId;
  }

  Result<> LibraryWriter::updateList(ListDraft const& draft)
  {
    auto txn = _implPtr->library.writeTransaction();
    auto listWriter = _implPtr->library.lists().writer(txn);
    auto optExisting = listWriter.get(draft.listId);

    if (!optExisting)
    {
      return makeError(Error::Code::NotFound, std::format("list not found: {}", draft.listId));
    }

    if (auto result = validateListDraft(_implPtr->library, listWriter, draft); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const payload = payloadForDraft(draft);

    if (std::ranges::equal(optExisting->rawData(), payload))
    {
      return {};
    }

    if (auto result = listWriter.update(draft.listId, payload); !result)
    {
      return storageError("Failed to update list", result.error());
    }

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit list update", result.error());
    }

    _implPtr->changes.notifyListsMutated({draft.listId}, {});
    return {};
  }

  bool LibraryWriter::deleteList(ListId listId)
  {
    auto txn = _implPtr->library.writeTransaction();

    if (!_implPtr->library.lists().writer(txn).remove(listId))
    {
      return false;
    }

    commitOrThrow(txn, "Failed to commit list delete");

    _implPtr->changes.notifyListsMutated({}, {listId});
    return true;
  }

  bool LibraryWriter::deleteTrack(TrackId trackId)
  {
    auto txn = _implPtr->library.writeTransaction();

    auto writer = _implPtr->library.tracks().writer(txn);
    auto optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return false;
    }

    auto const uri = std::string{optView->property().uri()};
    auto changedLists = removeTrackFromManualLists(_implPtr->library, txn, trackId);

    if (!uri.empty())
    {
      if (auto result = _implPtr->library.manifest().writer(txn).remove(uri); !result)
      {
        throwStorageError("Failed to remove file manifest entry", result.error());
      }
    }

    if (!writer.remove(trackId))
    {
      return false;
    }

    commitOrThrow(txn, "Failed to commit track delete");

    if (!changedLists.empty())
    {
      _implPtr->changes.notifyListsMutated(changedLists, {});
    }

    _implPtr->changes.notifyTrackCollectionChanged({}, {trackId});
    _implPtr->changes.notifyTracksMutated({trackId});
    return true;
  }

  Result<TrackId> LibraryWriter::createTrackFromFile(std::filesystem::path const& path)
  {
    auto const targetResult = importTargetForPath(_implPtr->library, path);

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

    auto txn = _implPtr->library.writeTransaction();
    auto writer = _implPtr->library.tracks().writer(txn);
    auto manifestWriter = _implPtr->library.manifest().writer(txn);

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

    auto preparedResult = builder.prepare(txn, _implPtr->library.dictionary(), _implPtr->library.resources());

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

    if (auto result = txn.commit(); !result)
    {
      return storageError("Failed to commit track creation", result.error());
    }

    _implPtr->changes.notifyTrackCollectionChanged({id}, {});
    _implPtr->changes.notifyTracksMutated({id});
    return id;
  }
} // namespace ao::rt
