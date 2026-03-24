// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <filesystem>
#include <rs/tag/Metadata.h>

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

    virtual Metadata loadMetadata() const = 0;

    virtual void saveMetadata(Metadata const& metadata) = 0;

  protected:
    boost::interprocess::file_mapping _fileMapping;
    boost::interprocess::mapped_region _mappedRegion;
  };
  // NOLINTEND(cppcoreguidelines-special-member-functions)
}