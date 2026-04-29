// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <cstddef>
#include <filesystem>
#include <rs/Error.h>
#include <span>
#include <string>

namespace rs::utility
{

  /**
   * @brief A safe wrapper around boost::interprocess for memory-mapped file access.
   */
  class MappedFile final
  {
  public:
    MappedFile() = default;
    ~MappedFile() = default;

    MappedFile(MappedFile const&) = delete;
    MappedFile& operator=(MappedFile const&) = delete;

    MappedFile(MappedFile&&) noexcept = default;
    MappedFile& operator=(MappedFile&&) noexcept = default;

    /**
     * @brief Maps the specified file into memory (read-only).
     * @param filePath The path to the file to map.
     * @return rs::Result<>::ok() on success, or an error with message on failure.
     */
    rs::Result<> map(std::filesystem::path const& filePath);

    /**
     * @brief Closes the mapping.
     */
    void unmap();

    /**
     * @brief Returns a read-only span of the mapped file content.
     */
    std::span<std::byte const> bytes() const;

    /**
     * @brief Returns true if a file is currently mapped.
     */
    bool isMapped() const;

  private:
    boost::interprocess::file_mapping _fileMapping;
    boost::interprocess::mapped_region _mappedRegion;
    bool _isMapped = false;
  };

} // namespace rs::utility