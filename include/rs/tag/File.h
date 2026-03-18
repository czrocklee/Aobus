// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <filesystem>
#include <rs/tag/Metadata.h>

namespace rs::tag
{
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

    virtual Metadata loadMetadata() const = 0;

    virtual void saveMetadata(Metadata const& metadata) = 0;

  protected:
    boost::interprocess::file_mapping _fileMapping;
    boost::interprocess::mapped_region _mappedRegion;
  };
}