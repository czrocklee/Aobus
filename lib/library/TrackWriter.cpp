// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/TrackWriter.h>

#include "TrackWrite.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/WriteTransaction.h>

#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::library
{
  namespace
  {
    constexpr auto kManifestPreflightTrackId = TrackId{1};

    Result<std::string> requireTrackUri(TrackStore::Writer const& writer, TrackId const id)
    {
      auto const optView = writer.get(id, TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("Track {} does not exist", id.raw()));
      }

      return std::string{optView->property().uri()};
    }

    void requireManifestBinding(FileManifestStore::Writer const& writer, std::string_view const uri, TrackId const id)
    {
      auto const optManifest = writer.get(uri);

      AO_INVARIANT(optManifest, "Track {} has no manifest binding for '{}' after library validation", id.raw(), uri);
      AO_INVARIANT(optManifest->trackId() == id,
                   "Manifest '{}' refers to Track {} instead of {} after library validation",
                   uri,
                   optManifest->trackId().raw(),
                   id.raw());
    }

    void throwAfterMutation(Error error)
    {
      lmdb::detail::throwTransactionFailure(std::move(error));
    }
  } // namespace

  TrackWriter::TrackWriter(WriteTransaction& transaction)
    : _transaction{&transaction}
  {
  }

  Result<> TrackWriter::validate(TrackBuilder const& track) const
  {
    requireActiveOperation();

    if (auto hotRes = track.validateHotSerializable(); !hotRes)
    {
      return hotRes;
    }

    if (track._baselineKind == TrackBuilder::BaselineKind::HotOnly)
    {
      return makeError(Error::Code::InvalidInput, "A hot-only Track builder cannot represent a complete Track");
    }

    if (auto coldRes = track.validateColdSerializable(); !coldRes)
    {
      return coldRes;
    }

    return validateResourceReferences(track);
  }

  Result<> TrackWriter::validateResourceReferences(TrackBuilder const& track) const
  {
    auto& writer = _transaction->resourceStoreWriter(_transaction->resourceStore());

    for (auto const& pending : track.coverArt().entries())
    {
      auto const* resourceId = std::get_if<ResourceId>(&pending.source);

      if (resourceId != nullptr && !writer.get(*resourceId))
      {
        return makeError(Error::Code::NotFound, std::format("Cover-art Resource {} does not exist", resourceId->raw()));
      }
    }

    return {};
  }

  std::optional<TrackView> TrackWriter::get(TrackId const id, TrackStore::Reader::LoadMode const mode) const
  {
    requireActiveOperation();
    return _transaction->trackStoreWriter().get(id, mode);
  }

  std::optional<FileManifestView> TrackWriter::manifest(std::string_view const uri) const
  {
    requireActiveOperation();
    return _transaction->manifestStoreWriter().get(uri);
  }

  Result<TrackId> TrackWriter::create(TrackBuilder const& track, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    if (auto resourceRes = validateResourceReferences(track); !resourceRes)
    {
      return std::unexpected{resourceRes.error()};
    }

    auto& manifestWriter = _transaction->manifestStoreWriter();
    // Creation allocates a nonzero Track id. Use one only to validate the
    // caller-owned manifest facts before Track preparation can stage effects.
    auto const preflightRes = manifestBuilder.trackId(kManifestPreflightTrackId).prepare(track.property().uri());

    if (!preflightRes)
    {
      return std::unexpected{preflightRes.error()};
    }

    if (manifestWriter.get(preflightRes->uri()))
    {
      return makeError(Error::Code::Conflict, std::format("Manifest '{}' already exists", preflightRes->uri()));
    }

    auto preparedTrackRes = track.prepare(*_transaction, _transaction->resourceStore());

    if (!preparedTrackRes)
    {
      return std::unexpected{preparedTrackRes.error()};
    }

    auto const& [hot, cold] = *preparedTrackRes;
    AO_INVARIANT(cold.uri() == preflightRes->uri(), "Track and manifest URI validation disagreed");
    auto& trackWriter = _transaction->trackStoreWriter();
    auto idRes = createPreparedTrackRecord(trackWriter, hot, cold);

    if (!idRes)
    {
      return std::unexpected{idRes.error()};
    }

    auto preparedManifestRes = manifestBuilder.trackId(*idRes).prepare(cold.uri());
    AO_INVARIANT(preparedManifestRes,
                 "Validated manifest failed preparation after Track id assignment: {}",
                 preparedManifestRes.error().message);

    if (auto putRes = manifestWriter.put(*preparedManifestRes); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return *idRes;
  }

  Result<> TrackWriter::update(TrackId const id, TrackBuilder const& track)
  {
    requireActiveOperation();

    auto& writer = _transaction->trackStoreWriter();
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    if (*currentUriRes != track.property().uri())
    {
      return makeError(Error::Code::InvalidInput, "A normal Track update cannot change its URI; use relink");
    }

    auto& manifestWriter = _transaction->manifestStoreWriter();

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    if (auto resourceRes = validateResourceReferences(track); !resourceRes)
    {
      return resourceRes;
    }

    auto preparedRes = track.prepare(*_transaction, _transaction->resourceStore());

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    auto const& [hot, cold] = *preparedRes;
    AO_INVARIANT(cold.uri() == *currentUriRes, "Validated Track update changed its URI");
    return updatePreparedTrackRecord(writer, id, hot, cold);
  }

  Result<> TrackWriter::updateHot(TrackId const id, TrackBuilder const& track)
  {
    requireActiveOperation();

    auto& writer = _transaction->trackStoreWriter();
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    auto& manifestWriter = _transaction->manifestStoreWriter();

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    auto preparedRes = track.prepareHot(*_transaction);

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    return updatePreparedHotTrackRecord(writer, id, *preparedRes);
  }

  Result<> TrackWriter::updateCold(TrackId const id, TrackBuilder const& track)
  {
    requireActiveOperation();

    auto& writer = _transaction->trackStoreWriter();
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    if (*currentUriRes != track.property().uri())
    {
      return makeError(Error::Code::InvalidInput, "A normal Track update cannot change its URI; use relink");
    }

    auto& manifestWriter = _transaction->manifestStoreWriter();

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    if (auto resourceRes = validateResourceReferences(track); !resourceRes)
    {
      return resourceRes;
    }

    auto preparedRes = track.prepareCold(*_transaction, _transaction->resourceStore());

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    AO_INVARIANT(preparedRes->uri() == *currentUriRes, "Validated cold Track update changed its URI");
    return updatePreparedColdTrackRecord(writer, id, *preparedRes);
  }

  Result<> TrackWriter::replace(TrackId const id, TrackBuilder const& track, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    auto& writer = _transaction->trackStoreWriter();
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    if (*currentUriRes != track.property().uri())
    {
      return makeError(Error::Code::InvalidInput, "A Track replacement cannot change its URI; use relink");
    }

    auto& manifestWriter = _transaction->manifestStoreWriter();

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    if (auto resourceRes = validateResourceReferences(track); !resourceRes)
    {
      return resourceRes;
    }

    auto preparedManifestRes = manifestBuilder.trackId(id).prepare(*currentUriRes);

    if (!preparedManifestRes)
    {
      return std::unexpected{preparedManifestRes.error()};
    }

    auto preparedTrackRes = track.prepare(*_transaction, _transaction->resourceStore());

    if (!preparedTrackRes)
    {
      return std::unexpected{preparedTrackRes.error()};
    }

    auto const& [hot, cold] = *preparedTrackRes;
    AO_INVARIANT(cold.uri() == *currentUriRes, "Validated Track replacement changed its URI");

    if (auto updateRes = updatePreparedTrackRecord(writer, id, hot, cold); !updateRes)
    {
      return updateRes;
    }

    if (auto putRes = manifestWriter.put(*preparedManifestRes); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return {};
  }

  Result<> TrackWriter::updateManifest(TrackId const id, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    auto& writer = _transaction->trackStoreWriter();
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    auto& manifestWriter = _transaction->manifestStoreWriter();

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    auto preparedRes = manifestBuilder.trackId(id).prepare(*currentUriRes);

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    return manifestWriter.put(*preparedRes);
  }

  Result<> TrackWriter::relink(TrackId const id, TrackBuilder const& track, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    auto& writer = _transaction->trackStoreWriter();
    auto oldUriRes = requireTrackUri(writer, id);

    if (!oldUriRes)
    {
      return std::unexpected{oldUriRes.error()};
    }

    if (*oldUriRes == track.property().uri())
    {
      return replace(id, track, std::move(manifestBuilder));
    }

    auto& manifestWriter = _transaction->manifestStoreWriter();

    requireManifestBinding(manifestWriter, *oldUriRes, id);

    auto preparedManifestRes = manifestBuilder.trackId(id).prepare(track.property().uri());

    if (!preparedManifestRes)
    {
      return std::unexpected{preparedManifestRes.error()};
    }

    if (manifestWriter.get(preparedManifestRes->uri()))
    {
      return makeError(Error::Code::Conflict, std::format("Manifest '{}' already exists", preparedManifestRes->uri()));
    }

    if (auto resourceRes = validateResourceReferences(track); !resourceRes)
    {
      return resourceRes;
    }

    auto preparedTrackRes = track.prepare(*_transaction, _transaction->resourceStore());

    if (!preparedTrackRes)
    {
      return std::unexpected{preparedTrackRes.error()};
    }

    auto const& [hot, cold] = *preparedTrackRes;
    AO_INVARIANT(cold.uri() == preparedManifestRes->uri(), "Track and manifest URI validation disagreed");

    if (auto updateRes = updatePreparedTrackRecord(writer, id, hot, cold); !updateRes)
    {
      return updateRes;
    }

    auto const removedOldManifest = manifestWriter.remove(*oldUriRes);
    AO_INVARIANT(removedOldManifest, "Validated old manifest binding disappeared during relink");

    if (auto putRes = manifestWriter.put(*preparedManifestRes); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return {};
  }

  Result<bool> TrackWriter::remove(TrackId const id)
  {
    requireActiveOperation();

    auto& writer = _transaction->trackStoreWriter();
    auto const optView = writer.get(id, TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return false;
    }

    auto const uri = std::string{optView->property().uri()};
    auto& manifestWriter = _transaction->manifestStoreWriter();
    requireManifestBinding(manifestWriter, uri, id);

    auto const removedManifest = manifestWriter.remove(uri);
    AO_INVARIANT(removedManifest, "Validated Track manifest binding disappeared during deletion");
    auto const removedTrack = writer.remove(id);
    AO_INVARIANT(removedTrack, "Validated Track disappeared during deletion");
    return true;
  }

  Result<> TrackWriter::clear()
  {
    requireActiveOperation();

    auto& manifestWriter = _transaction->manifestStoreWriter();

    if (auto manifestRes = manifestWriter.clear(); !manifestRes)
    {
      return manifestRes;
    }

    auto& trackWriter = _transaction->trackStoreWriter();

    if (auto trackRes = trackWriter.clear(); !trackRes)
    {
      throwAfterMutation(std::move(trackRes.error()));
    }

    return {};
  }

  void TrackWriter::requireActiveOperation() const
  {
    _transaction->requireOperationActive();
  }
} // namespace ao::library
