// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <catch2/catch_tostring.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

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

  enum class SymlinkType : std::uint8_t
  {
    File,
    Directory,
  };

  /**
   * Owns one test symlink and skips the current Catch2 case when the host
   * explicitly lacks symlink creation support or permission.
   */
  class [[nodiscard]] SymlinkFixture final
  {
  public:
    SymlinkFixture(std::filesystem::path target, std::filesystem::path link, SymlinkType type);
    ~SymlinkFixture() noexcept;

    SymlinkFixture(SymlinkFixture const&) = delete;
    SymlinkFixture& operator=(SymlinkFixture const&) = delete;
    SymlinkFixture(SymlinkFixture&&) = delete;
    SymlinkFixture& operator=(SymlinkFixture&&) = delete;

  private:
    std::filesystem::path _link;
    bool _created = false;
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
    bool isEffective() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  std::string formatFileTime(std::filesystem::file_time_type fileTime);

  /**
   * Converts a system-clock instant to the native file clock, floored to its
   * period. Retirement condition for the libc++ branch:
   * doc/development/macos-portability.md.
   */
  template<typename Duration>
  std::filesystem::file_time_type fileTimeFromSystemTime(std::chrono::sys_time<Duration> const time)
  {
    // Narrow before the epoch shift: nanoseconds since the 1601 Windows file
    // epoch overflow 64 bits, so a finer input must take the native period.
    auto const nativeTime = std::chrono::floor<std::filesystem::file_time_type::duration>(time);
// NOLINTNEXTLINE(misc-include-cleaner) -- each standard library defines the macro in a different header.
#if __cpp_lib_chrono >= 201907L
    auto const fileTime = std::chrono::clock_cast<std::chrono::file_clock>(nativeTime);
#else
    auto const fileTime = std::chrono::file_clock::from_sys(nativeTime);
#endif
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(fileTime);
  }
} // namespace ao::test

namespace Catch
{
  /**
   * Teaches Catch2 to print a file timestamp.
   *
   * Darwin counts std::filesystem::file_time_type in __int128 nanoseconds, and
   * no operator<< accepts that type, so Catch2's default stringifier fails to
   * compile as soon as a comparison of two file times is decomposed -- which it
   * is for every CHECK that compares them. Formatting the tick count by hand
   * keeps those assertions building, and reports the same text everywhere
   * rather than only fixing the platform that breaks.
   *
   * Retirement condition: doc/development/macos-portability.md.
   */
  template<>
  struct StringMaker<std::filesystem::file_time_type>
  {
    static std::string convert(std::filesystem::file_time_type const& fileTime)
    {
      return ao::test::formatFileTime(fileTime);
    }
  };
} // namespace Catch
