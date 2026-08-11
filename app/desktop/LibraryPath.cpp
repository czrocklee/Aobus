// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/LibraryPath.h>

#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <expected>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

namespace ao::desktop
{
  Result<std::filesystem::path> normalizeLibraryRoot(std::filesystem::path root)
  {
    if (root.empty())
    {
      return makeError(Error::Code::InvalidInput, "A library root cannot be empty");
    }

    auto error = std::error_code{};
    auto absolute = std::filesystem::absolute(std::move(root), error);

    if (error)
    {
      return makeError(Error::Code::IoError, std::format("Failed to resolve the library root: {}", error.message()));
    }

    return absolute.lexically_normal();
  }

  Result<std::filesystem::path> normalizeExistingLibraryRoot(std::filesystem::path root)
  {
    auto normalizedRes = normalizeLibraryRoot(std::move(root));

    if (!normalizedRes)
    {
      return std::unexpected{normalizedRes.error()};
    }

    auto error = std::error_code{};

    if (std::filesystem::is_directory(*normalizedRes, error))
    {
      return normalizedRes;
    }

    auto const displayPath = utility::pathToUtf8(*normalizedRes);

    if (error)
    {
      if (error == std::errc::no_such_file_or_directory)
      {
        return makeError(Error::Code::NotFound, std::format("Library root does not exist: '{}'", displayPath));
      }

      return makeError(
        Error::Code::IoError, std::format("Cannot access library root '{}': {}", displayPath, error.message()));
    }

    return makeError(Error::Code::NotFound, std::format("Library root is not a directory: '{}'", displayPath));
  }

  bool sameLibraryRoot(std::filesystem::path const& left, std::filesystem::path const& right)
  {
    auto error = std::error_code{};
    auto const equivalent = std::filesystem::equivalent(left, right, error);

    if (!error)
    {
      return equivalent;
    }

    auto leftError = std::error_code{};
    auto rightError = std::error_code{};
    auto const normalizedLeft = std::filesystem::absolute(left, leftError).lexically_normal();
    auto const normalizedRight = std::filesystem::absolute(right, rightError).lexically_normal();
    return !leftError && !rightError && normalizedLeft == normalizedRight;
  }
} // namespace ao::desktop
