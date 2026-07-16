// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryReader;
}

namespace ao::cli
{
  std::vector<TrackId> queryMatchingTrackIds(library::MusicLibrary const& library, std::string const& filter);
  std::vector<TrackId> requireTrackIds(rt::LibraryReader& reader, std::vector<std::uint32_t> const& rawIds);
} // namespace ao::cli
