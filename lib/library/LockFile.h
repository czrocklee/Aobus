// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::library::detail
{
  /**
   * @brief A non-blocking lock on a payload-free file, held until destruction.
   *
   * The lock covers the whole file: `flock` on POSIX and a full-range
   * `LockFileEx` on Windows. On the local filesystems a library supports, both
   * belong to the open file or handle, so two acquisitions in one process
   * exclude each other exactly as two processes do. A network filesystem may
   * emulate `flock` with per-process locks, which is one reason libraries do
   * not support them.
   */
  class LockFile final
  {
  public:
    enum class Mode : std::uint8_t
    {
      Shared,
      Exclusive,
    };

    /**
     * @brief Creates the file when it is missing and locks it without waiting.
     *
     * Fails as `ResourceBusy` while another holder's lock excludes `mode`,
     * and as `IoError` when the file cannot be opened or locked.
     */
    static Result<LockFile> acquire(std::filesystem::path const& path, Mode mode);

    ~LockFile();

    LockFile(LockFile const&) = delete;
    LockFile& operator=(LockFile const&) = delete;
    LockFile(LockFile&&) noexcept;
    LockFile& operator=(LockFile&&) noexcept;

  private:
    struct Impl;
    explicit LockFile(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::library::detail
