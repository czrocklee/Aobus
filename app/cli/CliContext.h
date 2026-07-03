// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Output.h"

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

  class CliContext final
  {
  public:
    explicit CliContext(std::ostream& out, std::ostream& err);
    ~CliContext();

    CliContext(CliContext const&) = delete;
    CliContext& operator=(CliContext const&) = delete;
    CliContext(CliContext&&) = delete;
    CliContext& operator=(CliContext&&) = delete;

    rt::CoreRuntime& runtime();
    library::MusicLibrary& musicLibrary();
    rt::Library& library();

    CliOptions& options() noexcept { return _options; }
    CliOptions const& options() const noexcept { return _options; }
    CliIo& io() noexcept { return _io; }
    CliIo const& io() const noexcept { return _io; }

  private:
    CliOptions _options;
    CliIo _io;
    std::unique_ptr<rt::CoreRuntime> _runtimePtr;
  };
} // namespace ao::cli
