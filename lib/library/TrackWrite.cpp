// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/TrackWrite.h>

#include "TrackRecordValidation.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/detail/LibraryError.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/TransactionFailure.h>

#include <cstdint>
#include <expected>
#include <format>
#include <utility>

namespace ao::library::detail
{
  class TrackWriteAccess final
  {
  public:
    static Result<TrackId> create(TrackStore::Writer& writer,
                                  TrackBuilder::PreparedHot const& preparedHot,
                                  TrackBuilder::PreparedCold const& preparedCold)
    {
      std::uint32_t rawTrackId = 0;

      {
        auto hotResult = writer._hotWriter.append(preparedHot.size());

        if (!hotResult)
        {
          return std::unexpected{hotResult.error()};
        }

        rawTrackId = hotResult->first;

        auto const hotBytes = hotResult->second;
        preparedHot.writeTo(hotBytes);

        if (auto validation = validateSerializedHotTrack(hotBytes); !validation)
        {
          throwLibraryError(Error::Code::InvalidState, "Prepared hot Track record is not canonical");
        }
      }

      auto coldResult = writer._coldWriter.create(rawTrackId, preparedCold.size());

      if (!coldResult)
      {
        auto error = std::move(coldResult.error());

        if (error.code == Error::Code::Conflict)
        {
          throwLibraryError(Error::Code::CorruptData,
                            std::format("Cold Track record {} already exists without its hot side", rawTrackId));
        }

        lmdb::throwTransactionFailure(std::move(error));
      }

      auto const coldBytes = *coldResult;
      preparedCold.writeTo(coldBytes);

      if (auto validation = validateSerializedColdTrack(coldBytes); !validation)
      {
        throwLibraryError(Error::Code::InvalidState, "Prepared cold Track record is not canonical");
      }

      return TrackId{rawTrackId};
    }

    static Result<> updateHot(TrackStore::Writer& writer, TrackId trackId, TrackBuilder::PreparedHot const& preparedHot)
    {
      if (auto validation = validateUpdateTarget(writer._hotWriter, trackId); !validation)
      {
        return validation;
      }

      replaceHot(writer, trackId, preparedHot);
      return {};
    }

    static Result<> updateCold(TrackStore::Writer& writer,
                               TrackId trackId,
                               TrackBuilder::PreparedCold const& preparedCold)
    {
      if (auto validation = validateUpdateTarget(writer._coldWriter, trackId); !validation)
      {
        return validation;
      }

      replaceCold(writer, trackId, preparedCold);
      return {};
    }

    static Result<> update(TrackStore::Writer& writer,
                           TrackId trackId,
                           TrackBuilder::PreparedHot const& preparedHot,
                           TrackBuilder::PreparedCold const& preparedCold)
    {
      if (auto validation = validateUpdateTarget(writer._hotWriter, trackId); !validation)
      {
        return validation;
      }

      replaceHot(writer, trackId, preparedHot);
      replaceCold(writer, trackId, preparedCold);
      return {};
    }

  private:
    static Result<> validateUpdateTarget(lmdb::Database::Writer const& target, TrackId const trackId)
    {
      if (trackId == kInvalidTrackId)
      {
        throwLibraryError(Error::Code::CorruptData, "Track zero is reserved");
      }

      if (!target.get(trackId.raw()))
      {
        return makeError(Error::Code::NotFound, std::format("Track {} does not exist", trackId.raw()));
      }

      return {};
    }

    static void replaceHot(TrackStore::Writer& writer,
                           TrackId const trackId,
                           TrackBuilder::PreparedHot const& preparedHot)
    {
      auto hotResult = writer._hotWriter.update(trackId.raw(), preparedHot.size());

      if (!hotResult)
      {
        lmdb::throwTransactionFailure(std::move(hotResult.error()));
      }

      auto const hotBytes = *hotResult;
      preparedHot.writeTo(hotBytes);

      if (auto validation = validateSerializedHotTrack(hotBytes); !validation)
      {
        throwLibraryError(Error::Code::InvalidState, "Prepared hot Track record is not canonical");
      }
    }

    static void replaceCold(TrackStore::Writer& writer,
                            TrackId const trackId,
                            TrackBuilder::PreparedCold const& preparedCold)
    {
      auto coldResult = writer._coldWriter.update(trackId.raw(), preparedCold.size());

      if (!coldResult)
      {
        lmdb::throwTransactionFailure(std::move(coldResult.error()));
      }

      auto const coldBytes = *coldResult;
      preparedCold.writeTo(coldBytes);

      if (auto validation = validateSerializedColdTrack(coldBytes); !validation)
      {
        throwLibraryError(Error::Code::InvalidState, "Prepared cold Track record is not canonical");
      }
    }
  };
} // namespace ao::library::detail

namespace ao::library
{
  Result<TrackId> createPreparedTrackRecord(TrackStore::Writer& writer,
                                            TrackBuilder::PreparedHot const& preparedHot,
                                            TrackBuilder::PreparedCold const& preparedCold)
  {
    return detail::TrackWriteAccess::create(writer, preparedHot, preparedCold);
  }

  Result<> updatePreparedHotTrackRecord(TrackStore::Writer& writer,
                                        TrackId trackId,
                                        TrackBuilder::PreparedHot const& preparedHot)
  {
    return detail::TrackWriteAccess::updateHot(writer, trackId, preparedHot);
  }

  Result<> updatePreparedColdTrackRecord(TrackStore::Writer& writer,
                                         TrackId trackId,
                                         TrackBuilder::PreparedCold const& preparedCold)
  {
    return detail::TrackWriteAccess::updateCold(writer, trackId, preparedCold);
  }

  Result<> updatePreparedTrackRecord(TrackStore::Writer& writer,
                                     TrackId trackId,
                                     TrackBuilder::PreparedHot const& preparedHot,
                                     TrackBuilder::PreparedCold const& preparedCold)
  {
    return detail::TrackWriteAccess::update(writer, trackId, preparedHot, preparedCold);
  }
} // namespace ao::library
