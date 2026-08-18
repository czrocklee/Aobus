// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ao::cli
{
  struct CliRunOptions final
  {
    std::uint64_t musicLibraryPinnedMapBytes = 0;

    /// Where derived caches live, or nothing to resolve the platform location.
    /// An in-process caller supplies one to keep the machine's own cover cache
    /// out of what it observes and out of what it evicts.
    std::optional<std::filesystem::path> optCacheDirectory{};
  };

  std::int32_t run(std::int32_t argc,
                   char const* const* argv,
                   std::ostream& out,
                   std::ostream& err,
                   CliRunOptions options = {});
  std::int32_t run(std::vector<std::string> const& args,
                   std::ostream& out,
                   std::ostream& err,
                   CliRunOptions options = {});
} // namespace ao::cli
