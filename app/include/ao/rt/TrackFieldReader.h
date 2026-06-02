// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/FileManifestStore.h>
#include <ao/rt/TrackField.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace ao::library
{
  class TrackView;
  class DictionaryStore;
}

namespace ao::rt
{
  using TrackFieldDuration = std::chrono::milliseconds;

  using TrackFieldRawValue =
    std::variant<std::monostate, std::string, std::uint16_t, std::uint32_t, std::uint64_t, TrackFieldDuration>;

  TrackFieldRawValue readTrackFieldRawValue(TrackField field,
                                            library::TrackView const& view,
                                            library::DictionaryStore const& dict,
                                            library::FileManifestStore::Reader const* manifestReader);
} // namespace ao::rt
