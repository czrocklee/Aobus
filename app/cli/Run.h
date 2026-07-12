// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace ao::cli
{
  struct CliRunOptions final
  {
    std::size_t musicLibraryMapSize = 0;
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
