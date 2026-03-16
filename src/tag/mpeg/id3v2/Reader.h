// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/tag/Metadata.h>

namespace rs::tag::mpeg::id3v2
{
  struct HeaderLayout;

  Metadata loadFrames(HeaderLayout const& header, void const* buffer, std::size_t size);
}