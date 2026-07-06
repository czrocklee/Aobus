// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// XXH3 wrappers for persisted non-security content identity. xxhash itself is
// an implementation detail of Xxh3.cpp; this header must stay free of
// <xxhash.h> types so library-layer headers never leak the dependency.
namespace ao::utility
{
  namespace detail
  {
    struct Xxh3State;
  } // namespace detail

  /// One-shot XXH3 64-bit hash, suitable for content-addressable keys.
  std::uint64_t xxh3Hash64(std::span<std::byte const> data) noexcept;

  std::uint64_t xxh3Hash64(std::string_view text) noexcept;

  /// Fixed-width 16-hex-digit form, suitable for stable keys and fingerprints.
  inline std::string xxh3Hash64Hex(std::string_view text)
  {
    return std::format("{:016x}", xxh3Hash64(text));
  }

  /// One-shot XXH3 128-bit hash for non-security content signatures. The
  /// returned bytes are in XXH128 canonical (big-endian) order, so the
  /// serialized form is platform independent.
  Hash128 xxh3Hash128(std::span<std::byte const> data) noexcept;

  Hash128 xxh3Hash128(std::string_view text) noexcept;

  inline std::string xxh3Hash128Hex(std::span<std::byte const> data)
  {
    return hash128Hex(xxh3Hash128(data));
  }

  inline std::string xxh3Hash128Hex(std::string_view text)
  {
    return hash128Hex(xxh3Hash128(text));
  }

  /// Incremental XXH3-128 for streamed non-security content signatures.
  ///
  /// Chunk-boundary invariant: mixing the same bytes in any chunking produces
  /// the same value as one-shot xxh3Hash128() over the concatenated input.
  /// value() returns the digest in XXH128 canonical (big-endian) byte order.
  class Xxh3Accumulator128 final
  {
  public:
    // Destructor and moves are defined out of line, where Xxh3State is a
    // complete type.
    Xxh3Accumulator128();
    ~Xxh3Accumulator128();
    Xxh3Accumulator128(Xxh3Accumulator128&& other) noexcept;
    Xxh3Accumulator128& operator=(Xxh3Accumulator128&& other) noexcept;

    Xxh3Accumulator128(Xxh3Accumulator128 const&) = delete;
    Xxh3Accumulator128& operator=(Xxh3Accumulator128 const&) = delete;

    void mix(std::span<std::byte const> bytes) noexcept;

    void mix(std::string_view bytes) noexcept;

    Hash128 value() const noexcept;

    std::string hex() const { return hash128Hex(value()); }

  private:
    std::unique_ptr<detail::Xxh3State> _statePtr;
  };

  /// Incremental XXH3 64-bit hash, for inputs that arrive in chunks (e.g.
  /// streamed files). Chunk-boundary invariant like the 128-bit accumulator.
  class Xxh3Accumulator64 final
  {
  public:
    // Destructor and moves are defined out of line, where Xxh3State is a
    // complete type.
    Xxh3Accumulator64();
    ~Xxh3Accumulator64();
    Xxh3Accumulator64(Xxh3Accumulator64&& other) noexcept;
    Xxh3Accumulator64& operator=(Xxh3Accumulator64&& other) noexcept;

    Xxh3Accumulator64(Xxh3Accumulator64 const&) = delete;
    Xxh3Accumulator64& operator=(Xxh3Accumulator64 const&) = delete;

    void mix(std::span<std::byte const> bytes) noexcept;

    void mix(std::string_view bytes) noexcept;

    std::uint64_t value() const noexcept;

    std::string hex() const { return std::format("{:016x}", value()); }

  private:
    std::unique_ptr<detail::Xxh3State> _statePtr;
  };
} // namespace ao::utility
