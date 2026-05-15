// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/MappedFile.h>

#include <ao/Error.h>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <span>

namespace ao::utility
{
  Result<> MappedFile::map(std::filesystem::path const& filePath)
  {
    unmap();

    try
    {
      _fileMapping = boost::interprocess::file_mapping(filePath.c_str(), boost::interprocess::read_only);         // NOLINT(misc-include-cleaner)
      _mappedRegion = boost::interprocess::mapped_region(_fileMapping, boost::interprocess::read_only);      // NOLINT(misc-include-cleaner)
      _isMapped = true;
      return {};
    }
    catch (std::exception const& e)
    {
      return std::unexpected(
        Error{.code = Error::Code::IoError, .message = std::format("Failed to mmap file: {}", e.what())});
    }
  }

  void MappedFile::unmap()
  {
    _isMapped = false;
    _mappedRegion = boost::interprocess::mapped_region{};
    _fileMapping = boost::interprocess::file_mapping{};
  }

  std::span<std::byte const> MappedFile::bytes() const
  {
    if (!_isMapped)
    {
      return {};
    }

    return {static_cast<std::byte const*>(_mappedRegion.get_address()), _mappedRegion.get_size()};
  }

  bool MappedFile::isMapped() const
  {
    return _isMapped;
  }
} // namespace ao::utility