// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/LibraryStartupPlan.h>

#include <ao/Error.h>
#include <ao/utility/Path.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/app/StartupOptions.h>

#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::winui
{
  namespace
  {
    Result<std::filesystem::path> absolutePath(std::filesystem::path root, std::string_view const context)
    {
      if (root.empty())
      {
        return makeError(Error::Code::InvalidInput, std::format("{} library root cannot be empty", context));
      }

      auto ec = std::error_code{};
      auto absolute = std::filesystem::absolute(std::move(root), ec);

      if (ec)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to resolve {} library root: {}", context, ec.message()));
      }

      return absolute.lexically_normal();
    }

    Result<std::filesystem::path> explicitRoot(std::filesystem::path root)
    {
      auto normalizedRes = absolutePath(std::move(root), "explicit");

      if (!normalizedRes)
      {
        return std::unexpected{normalizedRes.error()};
      }

      auto ec = std::error_code{};

      if (std::filesystem::is_directory(*normalizedRes, ec))
      {
        return normalizedRes;
      }

      if (ec)
      {
        if (ec == std::errc::no_such_file_or_directory)
        {
          return makeError(Error::Code::NotFound,
                           std::format("Explicit library root does not exist: '{}'", normalizedRes->generic_string()));
        }

        return makeError(
          Error::Code::IoError,
          std::format("Cannot access explicit library root '{}': {}", normalizedRes->generic_string(), ec.message()));
      }

      return makeError(Error::Code::NotFound,
                       std::format("Explicit library root is not a directory: '{}'", normalizedRes->generic_string()));
    }

    std::optional<std::filesystem::path> persistedRoot(std::string_view const value)
    {
      if (value.empty())
      {
        return std::nullopt;
      }

      try
      {
        auto normalizedRes = absolutePath(utility::pathFromUtf8(value), "persisted");

        if (!normalizedRes)
        {
          return std::nullopt;
        }

        if (auto ec = std::error_code{}; std::filesystem::is_directory(*normalizedRes, ec) && !ec)
        {
          return *normalizedRes;
        }
      }
      catch (std::filesystem::filesystem_error const&)
      {
        return std::nullopt;
      }

      return std::nullopt;
    }
  } // namespace

  void SelectedRootCommit::apply(DesktopSettings& settings) const
  {
    settings.lastLibraryPath = utility::pathToUtf8(root);
  }

  Result<LibraryStartupPlan> planLibraryStartup(StartupOptions const& options,
                                                DesktopSettings const& settings,
                                                std::filesystem::path emptyLibraryRoot)
  {
    if (options.optLibraryRoot)
    {
      auto rootRes = explicitRoot(*options.optLibraryRoot);

      if (!rootRes)
      {
        return std::unexpected{rootRes.error()};
      }

      return LibraryStartupPlan{.libraryRoot = *rootRes,
                                .source = LibraryStartupRootSource::Explicit,
                                .optSelectedRootCommit = SelectedRootCommit{.root = *rootRes}};
    }

    if (auto const optRoot = persistedRoot(settings.lastLibraryPath); optRoot)
    {
      return LibraryStartupPlan{.libraryRoot = *optRoot, .source = LibraryStartupRootSource::Persisted};
    }

    auto fallbackRes = absolutePath(std::move(emptyLibraryRoot), "empty-library fallback");

    if (!fallbackRes)
    {
      return std::unexpected{fallbackRes.error()};
    }

    return LibraryStartupPlan{.libraryRoot = *fallbackRes, .source = LibraryStartupRootSource::EmptyLibraryFallback};
  }

  void commitSelectedRoot(LibraryStartupPlan const& plan, DesktopSettings& settings)
  {
    if (plan.optSelectedRootCommit)
    {
      plan.optSelectedRootCommit->apply(settings);
    }
  }
} // namespace ao::winui
