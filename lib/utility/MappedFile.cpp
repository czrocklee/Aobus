// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/MappedFile.h>

#include <ao/Error.h>
#include <ao/utility/FileAccessPath.h>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <cstddef>
#include <expected>
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

    explicit Impl(std::filesystem::path const& path)
      : fileMapping{path.c_str(), boost::interprocess::read_only}
      , mappedRegion{fileMapping, boost::interprocess::read_only}
    {
    }
  };

  MappedFile::MappedFile() = default;

  MappedFile::~MappedFile() = default;

  MappedFile::MappedFile(MappedFile&&) noexcept = default;

  MappedFile& MappedFile::operator=(MappedFile&&) noexcept = default;

  Result<> MappedFile::map(std::filesystem::path const& filePath)
  {
    unmap();

    try
    {
      auto accessRes = fileAccessPath(filePath);

      if (!accessRes)
      {
        return std::unexpected{accessRes.error()};
      }

      _implPtr = std::make_unique<Impl>(*accessRes);
      return {};
    }
    catch (boost::interprocess::interprocess_exception const& e)
    {
      return makeError(Error::Code::IoError, std::format("Failed to mmap file: {}", e.what()));
    }
  }

  void MappedFile::unmap()
  {
    _implPtr.reset();
  }

  std::span<std::byte const> MappedFile::bytes() const
  {
    if (!_implPtr)
    {
      return {};
    }

    return {static_cast<std::byte const*>(_implPtr->mappedRegion.get_address()), _implPtr->mappedRegion.get_size()};
  }

  bool MappedFile::isMapped() const
  {
    return _implPtr != nullptr;
  }
} // namespace ao::utility
