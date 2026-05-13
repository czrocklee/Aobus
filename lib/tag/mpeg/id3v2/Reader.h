// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>

namespace ao::tag::mpeg::id3v2
{
  struct HeaderLayout;

  library::TrackBuilder loadFrames(TagFile const& owner,
                                   HeaderLayout const& header,
                                   void const* buffer,
                                   std::size_t size);
}