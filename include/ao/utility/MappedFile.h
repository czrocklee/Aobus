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
     *
     * Closes any previous mapping first. On failure the object is unmapped
     * and can be used again, just like a moved-from object.
     */
    Result<> map(std::filesystem::path const& filePath);

    /**
     * @brief Closes the mapping.
     */
    void unmap();

    /**
     * @brief Borrows the mapped bytes. A move transfers the mapping and its borrows;
     * releasing or replacing that mapping (including a failed remap) invalidates them.
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
