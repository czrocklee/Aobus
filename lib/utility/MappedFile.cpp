// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/utility/MappedFile.h>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <span>

namespace ao::utility
{
  struct MappedFile::Impl final
  {
    boost::interprocess::file_mapping fileMapping;
    boost::interprocess::mapped_region mappedRegion;
    bool isMapped = false;
  };

  MappedFile::MappedFile()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  MappedFile::~MappedFile() = default;

  MappedFile::MappedFile(MappedFile&&) noexcept = default;

  MappedFile& MappedFile::operator=(MappedFile&&) noexcept = default;

  Result<> MappedFile::map(std::filesystem::path const& filePath)
  {
    unmap();

    try
    {
      _implPtr->fileMapping = boost::interprocess::file_mapping{filePath.c_str(), boost::interprocess::read_only};
      _implPtr->mappedRegion =
        boost::interprocess::mapped_region{_implPtr->fileMapping, boost::interprocess::read_only};
      _implPtr->isMapped = true;
      return {};
    }
    catch (std::exception const& e)
    {
      return makeError(Error::Code::IoError, std::format("Failed to mmap file: {}", e.what()));
    }
  }

  void MappedFile::unmap()
  {
    _implPtr->isMapped = false;
    _implPtr->mappedRegion = boost::interprocess::mapped_region{};
    _implPtr->fileMapping = boost::interprocess::file_mapping{};
  }

  std::span<std::byte const> MappedFile::bytes() const
  {
    if (!_implPtr->isMapped)
    {
      return {};
    }

    return {static_cast<std::byte const*>(_implPtr->mappedRegion.get_address()), _implPtr->mappedRegion.get_size()};
  }

  bool MappedFile::isMapped() const
  {
    return _implPtr->isMapped;
  }
} // namespace ao::utility
