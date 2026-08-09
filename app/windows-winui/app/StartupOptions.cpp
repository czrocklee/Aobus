// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/StartupOptions.h>

#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <cstddef>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace ao::winui
{
  Result<StartupOptions> parseStartupOptions(std::span<std::string_view const> const arguments)
  {
    auto options = StartupOptions{};

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
      if (auto const argument = arguments[index]; argument != kLibraryRootOption)
      {
        return makeError(Error::Code::InvalidInput, std::format("Unknown WinUI startup argument '{}'", argument));
      }

      if (options.optLibraryRoot)
      {
        return makeError(Error::Code::InvalidInput,
                         std::format("WinUI startup argument '{}' may only be specified once", kLibraryRootOption));
      }

      if (index + 1 >= arguments.size() || arguments[index + 1].empty() || arguments[index + 1].starts_with('-'))
      {
        return makeError(
          Error::Code::InvalidInput, std::format("WinUI startup argument '{}' requires a path", kLibraryRootOption));
      }

      auto const value = arguments[++index];

      try
      {
        auto root = utility::pathFromUtf8(value);

        if (root.empty())
        {
          return makeError(
            Error::Code::InvalidInput, std::format("WinUI startup argument '{}' requires a path", kLibraryRootOption));
        }

        options.optLibraryRoot = std::move(root);
      }
      catch (std::filesystem::filesystem_error const& error)
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Invalid UTF-8 library root '{}': {}", value, error.what()));
      }
    }

    return options;
  }
} // namespace ao::winui
