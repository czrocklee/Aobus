// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Hash128.h>
#include <ao/utility/Xxh3.h>

// XXH_STATIC_LINKING_ONLY exposes the XXH3_state_t definition so the state
// can live behind the wrapper's pimpl. Header and library come from the same
// xxhash package, so the layout matches the linked implementation.
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace ao::utility
{
  namespace detail
  {
    struct Xxh3State final
    {
      XXH3_state_t value;
    };
  } // namespace detail

  namespace
  {
    Hash128 toCanonicalHash128(XXH128_hash_t hash) noexcept
    {
      // XXH128_canonicalFromHash defines the canonical serialization as
      // big-endian, which keeps stored signatures platform independent.
      auto canonical = XXH128_canonical_t{};
      ::XXH128_canonicalFromHash(&canonical, hash);

      auto result = Hash128{};
      static_assert(sizeof(canonical.digest) == sizeof(result.bytes));

      for (std::size_t index = 0; index < result.bytes.size(); ++index)
      {
        result.bytes[index] = static_cast<std::byte>(canonical.digest[index]);
      }

      return result;
    }

    std::unique_ptr<detail::Xxh3State> makeState()
    {
      auto statePtr = std::make_unique<detail::Xxh3State>();
      XXH3_INITSTATE(&statePtr->value);
      return statePtr;
    }
  } // namespace

  std::uint64_t xxh3Hash64(std::span<std::byte const> data) noexcept
  {
    return ::XXH3_64bits(data.data(), data.size());
  }

  std::uint64_t xxh3Hash64(std::string_view text) noexcept
  {
    return ::XXH3_64bits(text.data(), text.size());
  }

  Hash128 xxh3Hash128(std::span<std::byte const> data) noexcept
  {
    return toCanonicalHash128(::XXH3_128bits(data.data(), data.size()));
  }

  Hash128 xxh3Hash128(std::string_view text) noexcept
  {
    return toCanonicalHash128(::XXH3_128bits(text.data(), text.size()));
  }

  Xxh3Accumulator128::Xxh3Accumulator128()
    : _statePtr{makeState()}
  {
    ::XXH3_128bits_reset(&_statePtr->value);
  }

  Xxh3Accumulator128::~Xxh3Accumulator128() = default;
  Xxh3Accumulator128::Xxh3Accumulator128(Xxh3Accumulator128&& other) noexcept = default;
  Xxh3Accumulator128& Xxh3Accumulator128::operator=(Xxh3Accumulator128&& other) noexcept = default;

  void Xxh3Accumulator128::mix(std::span<std::byte const> bytes) noexcept
  {
    ::XXH3_128bits_update(&_statePtr->value, bytes.data(), bytes.size());
  }

  void Xxh3Accumulator128::mix(std::string_view bytes) noexcept
  {
    ::XXH3_128bits_update(&_statePtr->value, bytes.data(), bytes.size());
  }

  Hash128 Xxh3Accumulator128::value() const noexcept
  {
    return toCanonicalHash128(::XXH3_128bits_digest(&_statePtr->value));
  }

  Xxh3Accumulator64::Xxh3Accumulator64()
    : _statePtr{makeState()}
  {
    ::XXH3_64bits_reset(&_statePtr->value);
  }

  Xxh3Accumulator64::~Xxh3Accumulator64() = default;
  Xxh3Accumulator64::Xxh3Accumulator64(Xxh3Accumulator64&& other) noexcept = default;
  Xxh3Accumulator64& Xxh3Accumulator64::operator=(Xxh3Accumulator64&& other) noexcept = default;

  void Xxh3Accumulator64::mix(std::span<std::byte const> bytes) noexcept
  {
    ::XXH3_64bits_update(&_statePtr->value, bytes.data(), bytes.size());
  }

  void Xxh3Accumulator64::mix(std::string_view bytes) noexcept
  {
    ::XXH3_64bits_update(&_statePtr->value, bytes.data(), bytes.size());
  }

  std::uint64_t Xxh3Accumulator64::value() const noexcept
  {
    return ::XXH3_64bits_digest(&_statePtr->value);
  }
} // namespace ao::utility
