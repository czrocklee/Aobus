// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Database.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <utility>

namespace ao::lmdb::detail
{
  template<typename Encoder>
  concept ReservationEncoder = requires(Encoder&& encoder) {
    { std::invoke(std::forward<Encoder>(encoder), std::span<std::byte>{}) } noexcept -> std::same_as<void>;
  };

  /** Source-private zero-copy value encoding for the library Track writer. */
  class ReservationWriterAccess final
  {
  public:
    template<typename Encoder>
      requires ReservationEncoder<Encoder>
    static Result<> create(IntegerKeyDatabase::Writer& writer,
                           std::uint32_t const id,
                           std::size_t const size,
                           Encoder&& encoder)
    {
      return encodeReservation(writer.reserveCreate(id, size), std::forward<Encoder>(encoder));
    }

    template<typename Encoder>
      requires ReservationEncoder<Encoder>
    static Result<std::uint32_t> append(IntegerKeyDatabase::Writer& writer, std::size_t const size, Encoder&& encoder)
    {
      auto reservedRes = writer.reserveAppend(size);

      if (!reservedRes)
      {
        return std::unexpected{std::move(reservedRes.error())};
      }

      auto const [id, bytes] = *reservedRes;
      std::invoke(std::forward<Encoder>(encoder), std::span<std::byte>{bytes});
      return id;
    }

    template<typename Encoder>
      requires ReservationEncoder<Encoder>
    static Result<> update(IntegerKeyDatabase::Writer& writer,
                           std::uint32_t const id,
                           std::size_t const size,
                           Encoder&& encoder)
    {
      return encodeReservation(writer.reserveUpdate(id, size), std::forward<Encoder>(encoder));
    }

  private:
    template<typename Encoder>
    static Result<> encodeReservation(Result<std::span<std::byte>> reservationRes, Encoder&& encoder)
    {
      if (!reservationRes)
      {
        return std::unexpected{std::move(reservationRes.error())};
      }

      std::invoke(std::forward<Encoder>(encoder), std::span<std::byte>{*reservationRes});
      return {};
    }
  };
} // namespace ao::lmdb::detail
