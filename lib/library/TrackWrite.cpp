// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackWrite.h"

#include "TrackRecordValidation.h"
#include "lmdb/detail/ReservationWriterAccess.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <utility>

namespace ao::library::detail
{
  namespace
  {
    struct PreparedHotEncoder final
    {
      TrackBuilder::PreparedHot const& prepared;

      void operator()(std::span<std::byte> const bytes) const noexcept
      {
        prepared.writeTo(bytes);

        auto const validationRes = validateSerializedHotTrack(bytes);
        AO_ENSURES(validationRes,
                   "Prepared hot Track encoder produced a non-canonical record: {}",
                   validationRes.error().message);
      }
    };

    struct PreparedColdEncoder final
    {
      TrackBuilder::PreparedCold const& prepared;

      void operator()(std::span<std::byte> const bytes) const noexcept
      {
        prepared.writeTo(bytes);

        auto const validationRes = validateSerializedColdTrack(bytes);
        AO_ENSURES(validationRes,
                   "Prepared cold Track encoder produced a non-canonical record: {}",
                   validationRes.error().message);
      }
    };
  } // namespace

  class TrackWriteAccess final
  {
  public:
    static Result<TrackId> create(TrackStore::Writer& writer,
                                  TrackBuilder::PreparedHot const& preparedHot,
                                  TrackBuilder::PreparedCold const& preparedCold)
    {
      std::uint32_t rawTrackId = 0;

      {
        auto hotRes = lmdb::detail::ReservationWriterAccess::append(
          writer._hotWriter, preparedHot.size(), PreparedHotEncoder{preparedHot});

        if (!hotRes)
        {
          return std::unexpected{hotRes.error()};
        }

        rawTrackId = *hotRes;
      }

      auto coldRes = lmdb::detail::ReservationWriterAccess::create(
        writer._coldWriter, rawTrackId, preparedCold.size(), PreparedColdEncoder{preparedCold});

      if (!coldRes)
      {
        auto error = std::move(coldRes.error());
        AO_INVARIANT(error.code != Error::Code::Conflict,
                     "Cold Track record {} already exists without its hot side after library validation",
                     rawTrackId);
        lmdb::detail::throwTransactionFailure(std::move(error));
      }

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
    static Result<> validateUpdateTarget(lmdb::IntegerKeyDatabase::Writer const& target, TrackId const trackId)
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
      auto hotRes = lmdb::detail::ReservationWriterAccess::update(
        writer._hotWriter, trackId.raw(), preparedHot.size(), PreparedHotEncoder{preparedHot});

      if (!hotRes)
      {
        lmdb::detail::throwTransactionFailure(std::move(hotRes.error()));
      }
    }

    static void replaceCold(TrackStore::Writer& writer,
                            TrackId const trackId,
                            TrackBuilder::PreparedCold const& preparedCold)
    {
      auto coldRes = lmdb::detail::ReservationWriterAccess::update(
        writer._coldWriter, trackId.raw(), preparedCold.size(), PreparedColdEncoder{preparedCold});

      if (!coldRes)
      {
        lmdb::detail::throwTransactionFailure(std::move(coldRes.error()));
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
