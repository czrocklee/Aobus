// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/TrackWrite.h>

#include "TrackRecordValidation.h"
#include "detail/LibraryError.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>

#include <gsl-lite/gsl-lite.hpp>

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
        auto hotRes = writer._hotWriter.append(preparedHot.size());

        if (!hotRes)
        {
          return std::unexpected{hotRes.error()};
        }

        rawTrackId = hotRes->first;

        auto const hotBytes = hotRes->second;
        preparedHot.writeTo(hotBytes);

        auto const validationRes = validateSerializedHotTrack(hotBytes);
        gsl_Assert(validationRes && "Prepared hot Track record is not canonical");
      }

      auto coldRes = writer._coldWriter.create(rawTrackId, preparedCold.size());

      if (!coldRes)
      {
        auto error = std::move(coldRes.error());

        if (error.code == Error::Code::Conflict)
        {
          throwLibraryError(Error::Code::CorruptData,
                            std::format("Cold Track record {} already exists without its hot side", rawTrackId));
        }

        lmdb::detail::throwTransactionFailure(std::move(error));
      }

      auto const coldBytes = *coldRes;
      preparedCold.writeTo(coldBytes);

      auto const validationRes = validateSerializedColdTrack(coldBytes);
      gsl_Assert(validationRes && "Prepared cold Track record is not canonical");

      return TrackId{rawTrackId};
    }

    static Result<> updateHot(TrackStore::Writer& writer, TrackId trackId, TrackBuilder::PreparedHot const& preparedHot)
    {
      if (auto validationRes = validateUpdateTarget(writer._hotWriter, trackId); !validationRes)
      {
        return validationRes;
      }

      replaceHot(writer, trackId, preparedHot);
      return {};
    }

    static Result<> updateCold(TrackStore::Writer& writer,
                               TrackId trackId,
                               TrackBuilder::PreparedCold const& preparedCold)
    {
      if (auto validationRes = validateUpdateTarget(writer._coldWriter, trackId); !validationRes)
      {
        return validationRes;
      }

      replaceCold(writer, trackId, preparedCold);
      return {};
    }

    static Result<> update(TrackStore::Writer& writer,
                           TrackId trackId,
                           TrackBuilder::PreparedHot const& preparedHot,
                           TrackBuilder::PreparedCold const& preparedCold)
    {
      if (auto validationRes = validateUpdateTarget(writer._hotWriter, trackId); !validationRes)
      {
        return validationRes;
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
        return makeError(Error::Code::CorruptData, "Track zero is reserved");
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
      auto hotRes = writer._hotWriter.update(trackId.raw(), preparedHot.size());

      if (!hotRes)
      {
        lmdb::detail::throwTransactionFailure(std::move(hotRes.error()));
      }

      auto const hotBytes = *hotRes;
      preparedHot.writeTo(hotBytes);

      auto const validationRes = validateSerializedHotTrack(hotBytes);
      gsl_Assert(validationRes && "Prepared hot Track record is not canonical");
    }

    static void replaceCold(TrackStore::Writer& writer,
                            TrackId const trackId,
                            TrackBuilder::PreparedCold const& preparedCold)
    {
      auto coldRes = writer._coldWriter.update(trackId.raw(), preparedCold.size());

      if (!coldRes)
      {
        lmdb::detail::throwTransactionFailure(std::move(coldRes.error()));
      }

      auto const coldBytes = *coldRes;
      preparedCold.writeTo(coldBytes);

      auto const validationRes = validateSerializedColdTrack(coldBytes);
      gsl_Assert(validationRes && "Prepared cold Track record is not canonical");
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
