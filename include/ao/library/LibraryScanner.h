// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ao::library
{
  enum class ScanClassification : std::uint8_t
  {
    New,
    Changed,
    Missing,
    Unchanged,
    Unsupported,
    Error
  };

  struct ScanItem final
  {
    std::string uri;
    std::filesystem::path fullPath;
    ScanClassification classification = ScanClassification::Error;
    std::uint64_t fileSize = 0;
    std::uint64_t mtime = 0;
    TrackId trackId = kInvalidTrackId;
    std::string errorMessage = {};
  };

  struct ScanPlan final
  {
    std::vector<ScanItem> items;

    std::size_t count(ScanClassification classification) const
    {
      std::size_t count = 0;

      for (auto const& item : items)
      {
        if (item.classification == classification)
        {
          ++count;
        }
      }

      return count;
    }
  };

  class LibraryScanner final
  {
  public:
    using ProgressCallback = std::move_only_function<void(std::filesystem::path const& currentPath)>;

    explicit LibraryScanner(MusicLibrary& ml);

    /**
     * Scans the music root recursively and compares with the manifest.
     */
    ScanPlan buildPlan(ProgressCallback progress = nullptr);

  private:
    MusicLibrary& _ml;
  };
} // namespace ao::library
