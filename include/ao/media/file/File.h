// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

namespace ao::media::file
{
  class Visitor;

  struct PayloadView final
  {
    std::span<std::byte const> bytes;
    std::size_t offset = 0;
  };

  /**
   * Read-only supported audio file.
   *
   * File is move-only and supports sequential access. Its const accessors may
   * populate internal caches and must not be called concurrently. Views emitted
   * by visit() or audioPayload() remain valid while the backing File lives;
   * moving a File transfers that backing without copying it.
   */
  class [[nodiscard]] File final
  {
  public:
    static bool isSupported(std::filesystem::path const& path);
    static Result<File> open(std::filesystem::path const& path);

    ~File();
    File(File&&) noexcept;
    File& operator=(File&&) = delete;

    File(File const&) = delete;
    File& operator=(File const&) = delete;

    Result<> visit(Visitor& visitor) const;
    Result<PayloadView> audioPayload() const;

  private:
    struct Impl;

    explicit File(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::media::file
