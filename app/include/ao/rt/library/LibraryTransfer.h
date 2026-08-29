// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace ao::rt
{
  /** The interchange format version this build writes, and the only one it reads. */
  constexpr std::uint32_t kYamlFormatVersion = 5;

  enum class ExportMode : std::uint8_t
  {
    Delta,
    Metadata,
    Full,
    ListOnly,
  };

  constexpr std::string_view exportModeName(ExportMode const mode) noexcept
  {
    switch (mode)
    {
      case ExportMode::Delta: return "delta";
      case ExportMode::Metadata: return "metadata";
      case ExportMode::Full: return "full";
      case ExportMode::ListOnly: return "listOnly";
    }

    return {};
  }

  enum class ImportMode : std::uint8_t
  {
    Restore,
    Merge,
  };

  enum class ImportTargetScope : std::uint8_t
  {
    Library,
    Lists,
  };

  struct ImportReport final
  {
    std::uint32_t payloadVersion = 0;
    ExportMode payloadMode = ExportMode::Full;
    ImportTargetScope targetScope = ImportTargetScope::Library;
    std::uint64_t tracksCreated = 0;
    std::uint64_t tracksUpdated = 0;
    std::uint64_t tracksDeleted = 0;
    std::uint64_t listsCreated = 0;
    std::uint64_t listsDeleted = 0;
    std::uint64_t danglingReferencesIgnored = 0;

    bool operator==(ImportReport const&) const = default;
  };
} // namespace ao::rt
