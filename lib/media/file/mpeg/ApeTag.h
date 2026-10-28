// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <span>

namespace ao::media::file::mpeg
{
  // Locates the absolute start offset of a trailing APE tag within `bytes`,
  // whose end is expected to coincide with the tag footer.
  //
  // `bytes` is only the candidate range that ends at the expected footer; it
  // is not an audio boundary, and this helper is not complete metadata
  // validation. The contract is deliberately geometry-only: footer magic,
  // size-driven bounds, the footer header-present flag, and the header magic
  // when that flag is set. Tag version, reserved bytes, other flags,
  // header/footer field equality, and item data are out of scope; the two
  // current consumers keep different policies for those checks.
  Result<std::size_t> readApeTagStart(std::span<std::byte const> bytes);
} // namespace ao::media::file::mpeg
