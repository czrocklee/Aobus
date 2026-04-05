// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <filesystem>
#include <rs/tag/ParsedTrack.h>

namespace rs::tag
{
  // NOLINTBEGIN(cppcoreguidelines-special-member-functions)
  class File
  {
  public:
    enum class Mode
    {
      ReadOnly,
      ReadWrite
    };

    File(std::filesystem::path const& path, Mode mode);

    virtual ~File() = default;

    // Abstract base class - disable copy/move
    File(File const&) = delete;
    File& operator=(File const&) = delete;
    File(File&&) = delete;
    File& operator=(File&&) = delete;

    /**
     * Parse and return track metadata + embedded cover art.
     * embeddedCoverArt (if any) is a view into the mmap'd file — valid for
     * the lifetime of this File instance only.
     */
    virtual ParsedTrack loadTrack() const = 0;

    /**
     * Open a tag file by path, auto-detecting format from extension.
     * Returns nullptr for unsupported extensions.
     */
    static std::unique_ptr<File> open(std::filesystem::path const& path, Mode mode = Mode::ReadOnly);

  protected:
    boost::interprocess::file_mapping _fileMapping;
    boost::interprocess::mapped_region _mappedRegion;
  };
  // NOLINTEND(cppcoreguidelines-special-member-functions)
}