// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/library/TrackBuilder.h>
#include <rs/tag/File.h>

namespace rs::tag::mpeg::id3v2
{
  struct HeaderLayout;

  rs::library::TrackBuilder loadFrames(rs::tag::File const& owner,
                                       HeaderLayout const& header,
                                       void const* buffer,
                                       std::size_t size);
}