// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../../detail/Content.h"

#include <cstddef>
#include <optional>
#include <span>

namespace ao::media::file::mpeg::id3v2
{
  struct HeaderLayout;

  std::optional<detail::ContentBuilder> readFrames(HeaderLayout const& header, std::span<std::byte const> bytes);
} // namespace ao::media::file::mpeg::id3v2
