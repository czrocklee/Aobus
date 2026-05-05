// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/TrackBuilder.h>
#include <ao/tag/File.h>

namespace ao::tag::mpeg::id3v2
{
  struct HeaderLayout;

  ao::library::TrackBuilder loadFrames(ao::tag::File const& owner,
                                       HeaderLayout const& header,
                                       void const* buffer,
                                       std::size_t size);
}