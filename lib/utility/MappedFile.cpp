// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/utility/MappedFile.h>

#include <format>

namespace rs::utility
{

  std::string MappedFile::map(std::filesystem::path const& filePath)
  {
    unmap();

    try
    {
      _fileMapping = boost::interprocess::file_mapping(filePath.c_str(), boost::interprocess::read_only);
      _mappedRegion = boost::interprocess::mapped_region(_fileMapping, boost::interprocess::read_only);
      _isMapped = true;
      return {};
    }
    catch (std::exception const& e)
    {
      return std::format("Failed to mmap file: {}", e.what());
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

  bool MappedFile::isMapped() const { return _isMapped; }

} // namespace rs::utility