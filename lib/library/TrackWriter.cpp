// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/TrackWriter.h>

#include "LibraryUriValidation.h"
#include "TrackWrite.h"
#include "WriteTransactionAccess.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/AudioScalars.h>
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::library
{
  namespace
  {
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
      AO_INVARIANT(optManifest->trackId() == id || std::ranges::contains(optManifest->segmentTrackIds(), id),
                   "Manifest '{}' refers to Track {} instead of {} after library validation",
                   uri,
                   optManifest->trackId().raw(),
                   id.raw());
    }

    struct CarrierMember final
    {
      TrackId id{};
      TrackSegment segment{};
      SampleRate sampleRate{};
    };

    Result<> validateCarrier(std::vector<CarrierMember>& members, std::uint64_t totalFrames)
    {
      if (members.empty() || members.size() > kMaxTrackSegmentOrdinal || totalFrames == 0)
      {
        return makeError(Error::Code::InvalidInput, "Carrier must have 1..99 members and a positive extent");
      }

      std::ranges::sort(members, {}, [](CarrierMember const& member) { return member.segment.ordinal; });
      std::uint16_t previousOrdinal = 0;
      std::uint64_t previousEnd = 0;
      auto const rate = members.front().sampleRate;

      for (auto const& member : members)
      {
        auto const& segment = member.segment;

        if (segment.ordinal == 0 || segment.ordinal > kMaxTrackSegmentOrdinal || segment.ordinal <= previousOrdinal ||
            segment.startFrame < previousEnd || segment.startFrame > totalFrames || segment.frameCount == 0 ||
            segment.frameCount > totalFrames - segment.startFrame || member.sampleRate != rate ||
            !segmentDuration(segment.frameCount, rate))
        {
          return makeError(Error::Code::InvalidInput, "Carrier ranges, ordinals, extent, or sample rates disagree");
        }

        previousOrdinal = segment.ordinal;
        previousEnd = segment.startFrame + segment.frameCount;
      }

      return {};
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

  Result<> TrackWriter::validateStructure(TrackId id, TrackBuilder const& track, bool hotOnly) const
  {
    auto const optCurrent = get(id);

    if (!optCurrent)
    {
      return makeError(Error::Code::NotFound, "Track does not exist");
    }

    auto const property = optCurrent->property();
    auto const optSegment = property.segment();

    auto const consumesStructure = !hotOnly || track._baselineKind == TrackBuilder::BaselineKind::Complete;

    if ((consumesStructure && optSegment != track.property().segment()) ||
        (optSegment && (property.sampleRate() != track.property().sampleRate() ||
                        (consumesStructure && (property.uri() != track.property().uri() ||
                                               property.duration() != track.property().duration())))))
    {
      return makeError(
        Error::Code::InvalidInput, "Track mutation cannot change segment structure, URI, or sample rate");
    }

    return {};
  }

  Result<> TrackWriter::validateResourceReferences(TrackBuilder const& track) const
  {
    auto& writer = detail::WriteTransactionAccess::resourceStoreWriter(*_transaction, _transaction->resourceStore());

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
    return detail::WriteTransactionAccess::trackStoreWriter(*_transaction).get(id, mode);
  }

  std::optional<FileManifestView> TrackWriter::manifest(std::string_view const uri) const
  {
    requireActiveOperation();
    return detail::WriteTransactionAccess::manifestStoreWriter(*_transaction).get(uri);
  }

  Result<TrackId> TrackWriter::create(TrackBuilder const& track, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    if (track.property().segment())
    {
      return makeError(Error::Code::InvalidInput, "Segments must be created through createCarrier");
    }

    if (auto resourceRes = validateResourceReferences(track); !resourceRes)
    {
      return std::unexpected{resourceRes.error()};
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);
    // Manifest validation does not depend on the Track id that creation
    // allocates, so it completes before Track preparation can stage effects and
    // binds the real id only once the Track record exists.
    auto unboundManifestRes = manifestBuilder.validate(track.property().uri());

    if (!unboundManifestRes)
    {
      return std::unexpected{unboundManifestRes.error()};
    }

    if (manifestWriter.get(unboundManifestRes->uri()))
    {
      return makeError(Error::Code::Conflict, std::format("Manifest '{}' already exists", unboundManifestRes->uri()));
    }

    auto preparedTrackRes = track.prepare(*_transaction, _transaction->resourceStore());

    if (!preparedTrackRes)
    {
      return std::unexpected{preparedTrackRes.error()};
    }

    auto const& [hot, cold] = *preparedTrackRes;
    AO_INVARIANT(cold.uri() == unboundManifestRes->uri(), "Track and manifest URI validation disagreed");
    auto& trackWriter = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto idRes = createPreparedTrackRecord(trackWriter, hot, cold);

    if (!idRes)
    {
      throwAfterMutation(std::move(idRes.error()));
    }

    if (auto putRes = manifestWriter.put(std::move(*unboundManifestRes).bind(*idRes)); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return *idRes;
  }

  Result<std::vector<TrackId>> TrackWriter::createCarrier(std::span<TrackBuilder const> tracks,
                                                          FileManifestBuilder manifestBuilder,
                                                          std::uint64_t totalFrames)
  {
    requireActiveOperation();

    if (tracks.empty())
    {
      return makeError(Error::Code::InvalidInput, "A carrier needs members");
    }

    auto const uri = tracks.front().property().uri();
    auto unboundRes = manifestBuilder.validate(uri);

    if (!unboundRes)
    {
      return std::unexpected{unboundRes.error()};
    }

    // validate() canonicalizes the builder URI. Manifest lookup requires that
    // same spelling and aborts on any other text, so a noncanonical builder URI
    // is rejected here instead of being normalized or probed.
    if (uri != unboundRes->uri())
    {
      return makeError(Error::Code::InvalidInput, "Carrier URI is not canonical");
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

    if (manifestWriter.get(uri))
    {
      return makeError(Error::Code::Conflict, "Carrier URI already exists");
    }

    auto members = std::vector<CarrierMember>{};

    for (auto const& track : tracks)
    {
      if (auto validationRes = validate(track); !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      auto const optSegment = track.property().segment();

      if (!optSegment || track.property().uri() != uri)
      {
        return makeError(Error::Code::InvalidInput, "Carrier members must be segments of the same URI");
      }

      members.push_back({.segment = *optSegment, .sampleRate = track.property().sampleRate()});
    }

    if (auto validationRes = validateCarrier(members, totalFrames); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    auto prepared = std::vector<std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>>{};

    for (auto const& track : tracks)
    {
      auto preparedRes = track.prepare(*_transaction, _transaction->resourceStore());

      if (!preparedRes)
      {
        throwAfterMutation(std::move(preparedRes.error()));
      }

      prepared.push_back(std::move(*preparedRes));
    }

    auto ids = std::vector<TrackId>{};
    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);

    for (std::size_t index = 0; index < prepared.size(); ++index)
    {
      auto idRes = createPreparedTrackRecord(writer, prepared[index].first, prepared[index].second);

      if (!idRes)
      {
        throwAfterMutation(std::move(idRes.error()));
      }

      ids.push_back(*idRes);
      auto const optSegment = tracks[index].property().segment();
      AO_INVARIANT(optSegment, "Validated carrier builder lost its segment");
      auto const ordinal = optSegment->ordinal;
      std::ranges::find_if(members, [ordinal](auto const& member) { return member.segment.ordinal == ordinal; })->id =
        *idRes;
    }

    auto orderedIds = std::vector<TrackId>{};

    for (auto const& member : members)
    {
      orderedIds.push_back(member.id);
    }

    if (auto putRes = manifestWriter.put(std::move(*unboundRes).bindCarrier(orderedIds, totalFrames)); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return ids;
  }

  Result<std::vector<TrackId>> TrackWriter::addCarrierMembers(std::string_view uri,
                                                              std::span<TrackBuilder const> tracks)
  {
    requireActiveOperation();

    // Manifest lookup aborts on noncanonical text, so reject it as input first.
    if (!detail::isCanonicalLibraryUri(uri))
    {
      return makeError(Error::Code::InvalidInput, "Carrier URI is not canonical");
    }

    auto const optManifest = manifest(uri);

    if (!optManifest)
    {
      return makeError(Error::Code::NotFound, "Carrier does not exist");
    }

    if (optManifest->segmentCount() == 0)
    {
      return makeError(Error::Code::InvalidInput, "Cannot add segments to a whole-file Track");
    }

    auto manifestBuilder = FileManifestBuilder::fromView(*optManifest);
    auto const totalFrames = optManifest->totalFrames();
    auto members = std::vector<CarrierMember>{};

    for (auto id : optManifest->segmentTrackIds())
    {
      auto const optView = get(id);
      AO_INVARIANT(optView, "Validated carrier references a missing Track");
      auto const optSegment = optView->property().segment();
      AO_INVARIANT(optSegment, "Validated carrier member lost its segment");
      members.push_back({.id = id, .segment = *optSegment, .sampleRate = optView->property().sampleRate()});
    }

    for (auto const& track : tracks)
    {
      if (auto validationRes = validate(track); !validationRes)
      {
        return std::unexpected{validationRes.error()};
      }

      auto const optSegment = track.property().segment();

      if (!optSegment || track.property().uri() != uri)
      {
        return makeError(Error::Code::InvalidInput, "New carrier members must name the carrier URI");
      }

      members.push_back({.segment = *optSegment, .sampleRate = track.property().sampleRate()});
    }

    if (auto validationRes = validateCarrier(members, totalFrames); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    auto unboundRes = manifestBuilder.validate(uri);
    AO_INVARIANT(unboundRes);
    auto prepared = std::vector<std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>>{};

    for (auto const& track : tracks)
    {
      auto preparedRes = track.prepare(*_transaction, _transaction->resourceStore());

      if (!preparedRes)
      {
        throwAfterMutation(std::move(preparedRes.error()));
      }

      prepared.push_back(std::move(*preparedRes));
    }

    auto ids = std::vector<TrackId>{};
    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);

    for (std::size_t index = 0; index < prepared.size(); ++index)
    {
      auto idRes = createPreparedTrackRecord(writer, prepared[index].first, prepared[index].second);

      if (!idRes)
      {
        throwAfterMutation(std::move(idRes.error()));
      }

      ids.push_back(*idRes);
      auto const optSegment = tracks[index].property().segment();
      AO_INVARIANT(optSegment, "Validated carrier builder lost its segment");
      auto const ordinal = optSegment->ordinal;
      std::ranges::find_if(members, [ordinal](auto const& member) { return member.segment.ordinal == ordinal; })->id =
        *idRes;
    }

    auto orderedIds = std::vector<TrackId>{};

    for (auto const& member : members)
    {
      orderedIds.push_back(member.id);
    }

    if (auto putRes = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction)
                        .put(std::move(*unboundRes).bindCarrier(orderedIds, totalFrames));
        !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return ids;
  }

  Result<> TrackWriter::relinkCarrier(std::string_view oldUri,
                                      std::string_view newUri,
                                      FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    if (!detail::isCanonicalLibraryUri(oldUri))
    {
      return makeError(Error::Code::InvalidInput, "Carrier URI is not canonical");
    }

    auto const optManifest = manifest(oldUri);

    if (!optManifest)
    {
      return makeError(Error::Code::NotFound, "Carrier does not exist");
    }

    if (optManifest->segmentCount() == 0)
    {
      return makeError(Error::Code::InvalidInput, "Whole-file Track is not a carrier");
    }

    if (oldUri == newUri)
    {
      return updateManifest(oldUri, std::move(manifestBuilder));
    }

    auto unboundRes = manifestBuilder.validate(newUri);

    if (!unboundRes)
    {
      return std::unexpected{unboundRes.error()};
    }

    if (newUri != unboundRes->uri())
    {
      return makeError(Error::Code::InvalidInput, "Relink URI is not canonical");
    }

    if (manifest(newUri))
    {
      return makeError(Error::Code::Conflict, "Relink destination already exists");
    }

    auto const oldUriText = std::string{oldUri};
    auto ids = std::vector<TrackId>{optManifest->segmentTrackIds().begin(), optManifest->segmentTrackIds().end()};
    auto const totalFrames = optManifest->totalFrames();
    auto prepared = std::vector<TrackBuilder::PreparedCold>{};

    for (auto const id : ids)
    {
      auto const optView = get(id);
      AO_INVARIANT(optView, "Validated carrier references a missing Track");
      prepared.push_back(TrackBuilder::PreparedCold::relink(*optView, newUri));
    }

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);

    for (std::size_t index = 0; index < ids.size(); ++index)
    {
      if (auto updateRes = updatePreparedColdTrackRecord(writer, ids[index], prepared[index]); !updateRes)
      {
        throwAfterMutation(std::move(updateRes.error()));
      }
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);
    auto const removed = manifestWriter.tryRemove(oldUriText);
    AO_INVARIANT(removed);

    if (auto putRes = manifestWriter.put(std::move(*unboundRes).bindCarrier(ids, totalFrames)); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return {};
  }

  Result<> TrackWriter::updateManifest(std::string_view uri, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    if (!detail::isCanonicalLibraryUri(uri))
    {
      return makeError(Error::Code::InvalidInput, "Manifest URI is not canonical");
    }

    auto const optManifest = manifest(uri);

    if (!optManifest)
    {
      return makeError(Error::Code::NotFound, "Manifest does not exist");
    }

    auto unboundRes = manifestBuilder.validate(uri);

    if (!unboundRes)
    {
      return std::unexpected{unboundRes.error()};
    }

    auto const totalFrames = optManifest->totalFrames();
    auto ids = std::vector<TrackId>{optManifest->segmentTrackIds().begin(), optManifest->segmentTrackIds().end()};
    auto prepared = ids.empty() ? std::move(*unboundRes).bind(optManifest->trackId())
                                : std::move(*unboundRes).bindCarrier(ids, totalFrames);
    return detail::WriteTransactionAccess::manifestStoreWriter(*_transaction).put(prepared);
  }

  Result<> TrackWriter::update(TrackId const id, TrackBuilder const& track)
  {
    requireActiveOperation();

    if (auto structureRes = validateStructure(id, track); !structureRes)
    {
      return structureRes;
    }

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    if (*currentUriRes != track.property().uri())
    {
      return makeError(Error::Code::InvalidInput, "A normal Track update cannot change its URI; use relink");
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

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

    if (auto updateRes = updatePreparedTrackRecord(writer, id, hot, cold); !updateRes)
    {
      throwAfterMutation(std::move(updateRes.error()));
    }

    return {};
  }

  Result<> TrackWriter::updateHot(TrackId const id, TrackBuilder const& track)
  {
    requireActiveOperation();

    if (auto structureRes = validateStructure(id, track, true); !structureRes)
    {
      return structureRes;
    }

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    auto preparedRes = track.prepareHot(*_transaction);

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    if (auto updateRes = updatePreparedHotTrackRecord(writer, id, *preparedRes); !updateRes)
    {
      throwAfterMutation(std::move(updateRes.error()));
    }

    return {};
  }

  Result<> TrackWriter::updateCold(TrackId const id, TrackBuilder const& track)
  {
    requireActiveOperation();

    if (auto structureRes = validateStructure(id, track); !structureRes)
    {
      return structureRes;
    }

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    if (*currentUriRes != track.property().uri())
    {
      return makeError(Error::Code::InvalidInput, "A normal Track update cannot change its URI; use relink");
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

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

    if (auto updateRes = updatePreparedColdTrackRecord(writer, id, *preparedRes); !updateRes)
    {
      throwAfterMutation(std::move(updateRes.error()));
    }

    return {};
  }

  Result<> TrackWriter::replace(TrackId const id, TrackBuilder const& track, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    if (auto structureRes = validateStructure(id, track); !structureRes)
    {
      return structureRes;
    }

    if (track.property().segment())
    {
      auto const uri = std::string{track.property().uri()};
      auto unboundRes = manifestBuilder.validate(uri);

      if (!unboundRes)
      {
        return std::unexpected{unboundRes.error()};
      }

      auto const optCurrent = manifest(uri);
      AO_INVARIANT(optCurrent, "Validated segment Track has no carrier manifest");
      auto const preparedManifest =
        std::move(*unboundRes).bindCarrier(optCurrent->segmentTrackIds(), optCurrent->totalFrames());

      if (auto updateRes = update(id, track); !updateRes)
      {
        return updateRes;
      }

      if (auto putRes = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction).put(preparedManifest);
          !putRes)
      {
        throwAfterMutation(std::move(putRes.error()));
      }

      return {};
    }

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    if (*currentUriRes != track.property().uri())
    {
      return makeError(Error::Code::InvalidInput, "A Track replacement cannot change its URI; use relink");
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    if (auto resourceRes = validateResourceReferences(track); !resourceRes)
    {
      return resourceRes;
    }

    auto unboundManifestRes = manifestBuilder.validate(*currentUriRes);

    if (!unboundManifestRes)
    {
      return std::unexpected{unboundManifestRes.error()};
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
      throwAfterMutation(std::move(updateRes.error()));
    }

    if (auto putRes = manifestWriter.put(std::move(*unboundManifestRes).bind(id)); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return {};
  }

  Result<> TrackWriter::updateManifest(TrackId const id, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto currentUriRes = requireTrackUri(writer, id);

    if (!currentUriRes)
    {
      return std::unexpected{currentUriRes.error()};
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

    requireManifestBinding(manifestWriter, *currentUriRes, id);

    return updateManifest(*currentUriRes, std::move(manifestBuilder));
  }

  Result<> TrackWriter::relink(TrackId const id, TrackBuilder const& track, FileManifestBuilder manifestBuilder)
  {
    requireActiveOperation();

    if (auto structureRes = validateStructure(id, track); !structureRes)
    {
      return structureRes;
    }

    auto const optCurrent = get(id);
    AO_INVARIANT(optCurrent, "Validated relink target disappeared");

    if (optCurrent->property().segment())
    {
      return makeError(Error::Code::InvalidInput, "A segment must be relinked through relinkCarrier");
    }

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto oldUriRes = requireTrackUri(writer, id);

    if (!oldUriRes)
    {
      return std::unexpected{oldUriRes.error()};
    }

    if (*oldUriRes == track.property().uri())
    {
      return replace(id, track, std::move(manifestBuilder));
    }

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

    requireManifestBinding(manifestWriter, *oldUriRes, id);

    auto unboundManifestRes = manifestBuilder.validate(track.property().uri());

    if (!unboundManifestRes)
    {
      return std::unexpected{unboundManifestRes.error()};
    }

    if (manifestWriter.get(unboundManifestRes->uri()))
    {
      return makeError(Error::Code::Conflict, std::format("Manifest '{}' already exists", unboundManifestRes->uri()));
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
    AO_INVARIANT(cold.uri() == unboundManifestRes->uri(), "Track and manifest URI validation disagreed");

    if (auto updateRes = updatePreparedTrackRecord(writer, id, hot, cold); !updateRes)
    {
      throwAfterMutation(std::move(updateRes.error()));
    }

    auto const removedOldManifest = manifestWriter.tryRemove(*oldUriRes);
    AO_INVARIANT(removedOldManifest, "Validated old manifest binding disappeared during relink");

    if (auto putRes = manifestWriter.put(std::move(*unboundManifestRes).bind(id)); !putRes)
    {
      throwAfterMutation(std::move(putRes.error()));
    }

    return {};
  }

  Result<bool> TrackWriter::remove(TrackId const id)
  {
    requireActiveOperation();

    auto& writer = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);
    auto const optView = writer.get(id, TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      return false;
    }

    auto const uri = std::string{optView->property().uri()};
    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);
    requireManifestBinding(manifestWriter, uri, id);

    auto const optManifest = manifestWriter.get(uri);
    AO_INVARIANT(optManifest, "Validated Track has no manifest binding");
    auto ids = std::vector<TrackId>{optManifest->segmentTrackIds().begin(), optManifest->segmentTrackIds().end()};
    auto const totalFrames = optManifest->totalFrames();
    auto manifestBuilder = FileManifestBuilder::fromView(*optManifest);
    std::erase(ids, id);

    if (ids.empty())
    {
      auto const removedManifest = manifestWriter.tryRemove(uri);
      AO_INVARIANT(removedManifest, "Validated Track manifest binding disappeared during deletion");
    }
    else
    {
      auto unboundRes = manifestBuilder.validate(uri);
      AO_INVARIANT(unboundRes);

      if (auto putRes = manifestWriter.put(std::move(*unboundRes).bindCarrier(ids, totalFrames)); !putRes)
      {
        throwAfterMutation(std::move(putRes.error()));
      }
    }

    auto const removedTrack = writer.tryRemove(id);
    AO_INVARIANT(removedTrack, "Validated Track disappeared during deletion");
    return true;
  }

  Result<> TrackWriter::clear()
  {
    requireActiveOperation();

    auto& manifestWriter = detail::WriteTransactionAccess::manifestStoreWriter(*_transaction);

    if (auto manifestRes = manifestWriter.clear(); !manifestRes)
    {
      return manifestRes;
    }

    auto& trackWriter = detail::WriteTransactionAccess::trackStoreWriter(*_transaction);

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
