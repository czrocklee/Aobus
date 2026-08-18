// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/utility/Sha256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace ao::library
{
  /**
   * ResourceDescriptor - the persisted `resources` row.
   *
   * The row names content and holds none of it. `digest` is the whole identity:
   * bytes are accepted as this resource if and only if they hash to it, from a
   * cache entry or a media file alike. `byteLength` is a stored fact about that
   * content rather than a component of its identity, so a caller can report a
   * cover's size without holding its bytes.
   *
   * `byteLength` admits nothing. A figure nothing verified must never decide
   * whether content is read, so a size ceiling applies to materialized bytes and
   * never to this field.
   *
   * Layout: 32-byte digest then a 4-byte length, which is 36 bytes with 4-byte
   * alignment and no padding. The asserts below pin that, and the row on disk is
   * this object representation rather than an encoding of it: a write hands the
   * store `utility::bytes::view` of the record, exactly as every other record in
   * this library is written. The length therefore sits in the machine's own byte
   * order, which costs nothing a database file did not already cost — LMDB binds
   * one to the architecture that created it.
   */
  struct ResourceDescriptor final
  {
    utility::Sha256Digest digest{};
    std::uint32_t byteLength{};
  };

  constexpr std::size_t kResourceDescriptorSize = 36;
  constexpr std::size_t kResourceDescriptorAlignment = 4;

  static_assert(sizeof(ResourceDescriptor) == kResourceDescriptorSize, "ResourceDescriptor must be exactly 36 bytes");
  static_assert(alignof(ResourceDescriptor) == kResourceDescriptorAlignment,
                "ResourceDescriptor must have 4-byte alignment");
  static_assert(std::is_trivially_copyable_v<ResourceDescriptor>, "ResourceDescriptor must be trivially copyable");
  static_assert(std::is_standard_layout_v<ResourceDescriptor>, "ResourceDescriptor must have standard layout");

  /**
   * @brief Whether content of @p byteLength has a describable length.
   *
   * Content longer than the row's 32-bit length field is refused rather than
   * truncated, and the refusal is named here so it can be exercised for a size
   * no test can hold: the store takes bytes, and a span claiming four gibibytes
   * would claim a range that does not exist.
   */
  constexpr bool resourceByteLengthFits(std::size_t const byteLength) noexcept
  {
    return byteLength <= std::numeric_limits<std::uint32_t>::max();
  }

  /**
   * @brief The 32-bit handle a track record stores for @p digest.
   *
   * A digest is 32 bytes and `CoverArtEntry` is 8, so the track record cannot
   * carry one. This is the compact local handle for it, and `resources` is the
   * table that resolves the handle back to the descriptor.
   *
   * The derivation is persisted, so it is pinned rather than described: the
   * first four bytes of the digest, in the order SHA-256 produces them, read as
   * a big-endian unsigned 32-bit integer, with a result of zero normalized to
   * `1` because zero is the reserved invalid id.
   *
   * Handles collide where digests do not, which is what the store's probe is
   * for; this function names the initial key of that probe.
   */
  constexpr ResourceId deriveResourceId(utility::Sha256Digest const& digest) noexcept
  {
    std::uint32_t raw = 0;

    for (std::size_t byteIndex = 0; byteIndex < sizeof(std::uint32_t); ++byteIndex)
    {
      raw = (raw << 8U) | static_cast<std::uint32_t>(digest.bytes[byteIndex]);
    }

    return ResourceId{raw == 0 ? 1U : raw};
  }

  /**
   * @brief The descriptor @p row holds, absent unless it is exactly a
   *        descriptor's width.
   *
   * Only the read direction is named. Writing is `utility::bytes::view` of the
   * record and needs no function, but reading has a rule that is not about
   * layout: a row of any other width is not a descriptor, and both callers turn
   * that into a corrupt-database verdict.
   *
   * The bytes are copied rather than viewed in place, which `bytes::tryLayout`
   * would do. That is deliberate twice over: the descriptor outlives the read
   * transaction that owns the row, and a view would also refuse an unaligned
   * row, reporting a sound database as corrupt for a reason that has nothing to
   * do with its contents.
   */
  std::optional<ResourceDescriptor> parseResourceDescriptor(std::span<std::byte const> row) noexcept;
} // namespace ao::library
