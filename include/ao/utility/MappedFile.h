// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

namespace ao::utility
{
  /**
   * @brief A safe wrapper around boost::interprocess for memory-mapped file access.
   *
   * The boost::interprocess mapping state is held behind a pimpl so that the
   * boost::interprocess headers stay out of this public header.
   */
  class MappedFile final
  {
  public:
    MappedFile();
    ~MappedFile();

    MappedFile(MappedFile const&) = delete;
    MappedFile& operator=(MappedFile const&) = delete;

    MappedFile(MappedFile&&) noexcept;
    MappedFile& operator=(MappedFile&&) noexcept;

    /**
     * @brief Maps the specified file into memory (read-only).
     * @param filePath The path to the file to map.
     * @return Result<>::ok() on success, or an error with message on failure.
     */
    Result<> map(std::filesystem::path const& filePath);

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
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::utility