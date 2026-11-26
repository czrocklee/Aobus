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
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/detail/TrackViewRawAccess.h>
#include <ao/lmdb/Database.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::library::detail
{
  namespace
  {
    std::optional<std::uint16_t> updateNumber(std::uint16_t& current, std::optional<std::uint16_t> optValue) noexcept
    {
      if (!optValue || *optValue == current)
      {
        return std::nullopt;
      }

      return std::exchange(current, *optValue);
    }

    template<typename Header>
    std::vector<std::byte> copyWithHeader(std::span<std::byte const> source, Header const& header)
    {
      AO_INVARIANT(source.size() >= sizeof(Header));
      auto bytes = std::vector<std::byte>{source.begin(), source.end()};
      std::memcpy(bytes.data(), &header, sizeof(header));
      return bytes;
    }

    // The source is an owned copy from this writer's gated row. Only fixed-width
    // numbers changed; every reference, structural field and payload is intact.
    void replaceNumberedSide(lmdb::IntegerKeyDatabase::Writer& writer, TrackId id, std::span<std::byte const> source)
    {
      auto const encode = [source](std::span<std::byte> destination) noexcept
      {
        AO_INVARIANT(destination.size() == source.size());
        std::memcpy(destination.data(), source.data(), source.size());
      };
      auto res = lmdb::detail::ReservationWriterAccess::update(writer, id.raw(), source.size(), encode);

      if (!res)
      {
        lmdb::detail::throwTransactionFailure(std::move(res.error()));
      }
    }

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

    static Result<TrackNumberPatch> updateNumbers(TrackStore::Writer& writer, TrackId id, TrackNumberPatch const& patch)
    {
      if (id == kInvalidTrackId)
      {
        return makeError(Error::Code::CorruptData, "Track zero is reserved");
      }

      using LoadMode = TrackStore::Reader::LoadMode;
      auto const needsCold = patch.optTrackNumber || patch.optTrackTotal || patch.optDiscNumber || patch.optDiscTotal;
      auto mode = LoadMode::Hot; // Also checks existence for an empty patch.

      if (needsCold)
      {
        mode = patch.optYear ? LoadMode::Both : LoadMode::Cold;
      }

      auto const optView = writer.get(id, mode);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("Track {} does not exist", id.raw()));
      }

      auto previous = TrackNumberPatch{};
      auto hot = std::vector<std::byte>{};
      auto cold = std::vector<std::byte>{};

      if (patch.optYear)
      {
        auto const bytes = TrackViewRawAccess::hotData(*optView);
        auto header = *utility::layout::view<TrackHotHeader>(bytes);
        previous.optYear = updateNumber(header.year, patch.optYear);

        if (previous.optYear)
        {
          hot = copyWithHeader(bytes, header);
        }
      }

      if (needsCold)
      {
        auto const bytes = TrackViewRawAccess::coldData(*optView);
        auto header = *utility::layout::view<TrackColdHeader>(bytes);
        previous.optTrackNumber = updateNumber(header.trackNumber, patch.optTrackNumber);
        previous.optTrackTotal = updateNumber(header.trackTotal, patch.optTrackTotal);
        previous.optDiscNumber = updateNumber(header.discNumber, patch.optDiscNumber);
        previous.optDiscTotal = updateNumber(header.discTotal, patch.optDiscTotal);

        if (previous.optTrackNumber || previous.optTrackTotal || previous.optDiscNumber || previous.optDiscTotal)
        {
          cold = copyWithHeader(bytes, header);
        }
      }

      // A reservation can invalidate either borrowed half, even in another DBI.
      if (!hot.empty())
      {
        replaceNumberedSide(writer._hotWriter, id, hot);
      }

      if (!cold.empty())
      {
        replaceNumberedSide(writer._coldWriter, id, cold);
      }

      return previous;
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

  Result<TrackNumberPatch> updateTrackRecordNumbers(TrackStore::Writer& writer,
                                                    TrackId trackId,
                                                    TrackNumberPatch const& patch)
  {
    return detail::TrackWriteAccess::updateNumbers(writer, trackId, patch);
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
