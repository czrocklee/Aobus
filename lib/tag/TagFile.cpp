// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/tag/TagFile.h>

#include <boost/interprocess/detail/os_file_functions.hpp>
#include <boost/interprocess/file_mapping.hpp>

#include <filesystem>

namespace ao::tag
{
  namespace
  {
    boost::interprocess::mode_t fromMode(TagFile::Mode mode)
    {
      return mode == TagFile::Mode::ReadOnly ? boost::interprocess::read_only : boost::interprocess::read_write;
    }
  }

  TagFile::TagFile(std::filesystem::path const& path, Mode mode)
    : _fileMapping{path.c_str(), fromMode(mode)}, _mappedRegion{_fileMapping, fromMode(mode)}
  {
  }
}