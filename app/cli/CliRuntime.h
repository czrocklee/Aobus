// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Output.h"

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class CoreRuntime;
  class Library;
}

namespace ao::cli
{
  struct CliOptions final
  {
    std::filesystem::path root{"."};
    OutputFormat format = OutputFormat::Plain;
  };

  struct CliIo final
  {
    std::ostream& out;
    std::ostream& err;
  };

  class CliRuntime final
  {
  public:
    explicit CliRuntime(std::ostream& out, std::ostream& err, std::size_t musicLibraryMapSize = 0);
    ~CliRuntime();

    CliRuntime(CliRuntime const&) = delete;
    CliRuntime& operator=(CliRuntime const&) = delete;
    CliRuntime(CliRuntime&&) = delete;
    CliRuntime& operator=(CliRuntime&&) = delete;

    rt::CoreRuntime& core();
    library::MusicLibrary& musicLibrary();
    rt::Library& library();

    CliOptions& options() noexcept { return _options; }
    CliOptions const& options() const noexcept { return _options; }
    CliIo& io() noexcept { return _io; }
    CliIo const& io() const noexcept { return _io; }

  private:
    CliOptions _options;
    CliIo _io;
    std::size_t _musicLibraryMapSize = 0;
    std::unique_ptr<rt::CoreRuntime> _runtimePtr;
  };
} // namespace ao::cli
