// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/tag/File.h>

namespace rs::tag
{
  namespace
  {
    boost::interprocess::mode_t fromMode(File::Mode mode)
    {
      return mode == File::Mode::ReadOnly ? boost::interprocess::read_only : boost::interprocess::read_write;
    }
  }

  File::File(std::filesystem::path const& path, Mode mode)
    : _fileMapping{path.c_str(), fromMode(mode)}, _mappedRegion{_fileMapping, fromMode(mode)}
  {
  }
}