// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::test
{
  /**
   * Verifies the platform's private managed-file policy used by AtomicFile.
   * Throws when the security metadata cannot be inspected.
   */
  bool hasPrivateManagedFileAccess(std::filesystem::path const& path);

  enum class DeniedDirectoryAccess : std::uint8_t
  {
    Read,
    Write,
  };

  /**
   * Temporarily denies one kind of access to a test directory and restores the
   * original platform permissions on destruction.
   */
  class [[nodiscard]] ScopedDirectoryAccessGuard final
  {
  public:
    ScopedDirectoryAccessGuard(std::filesystem::path path, DeniedDirectoryAccess access);
    ~ScopedDirectoryAccessGuard() noexcept;

    ScopedDirectoryAccessGuard(ScopedDirectoryAccessGuard const&) = delete;
    ScopedDirectoryAccessGuard& operator=(ScopedDirectoryAccessGuard const&) = delete;
    ScopedDirectoryAccessGuard(ScopedDirectoryAccessGuard&&) = delete;
    ScopedDirectoryAccessGuard& operator=(ScopedDirectoryAccessGuard&&) = delete;

    /**
     * Returns false when the current process can bypass the applied restriction
     * (for example, a root process bypassing POSIX mode bits).
     */
    bool effective() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::test
