// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "theme/WindowsThemeCoordinator.h"

#include "platform/WindowsStringResources.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/uimodel/preference/WindowsTheme.h>
#include <ao/utility/Path.h>
#include <ao/yaml/RymlAdapter.h>

#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <utility>

namespace ao::winui
{
  namespace
  {
    constexpr std::size_t kMaximumThemeBytes = 1'048'576;
  }

  WindowsThemeCoordinator::WindowsThemeCoordinator(std::filesystem::path themePath)
    : _themePath{std::move(themePath)}
  {
  }

  Result<uimodel::WindowsTheme> WindowsThemeCoordinator::reload()
  {
    if (!std::filesystem::exists(_themePath))
    {
      return makeError(
        Error::Code::NotFound, formatResource("ThemeFileNotFoundFormat", utility::pathToUtf8(_themePath)));
    }

    auto buffer = yaml::readFileResult(_themePath, kMaximumThemeBytes);

    if (!buffer)
    {
      return std::unexpected{buffer.error()};
    }

    try
    {
      auto errorState = yaml::ErrorCallbackState{utility::pathToUtf8(_themePath)};
      auto tree = ryml::Tree{yaml::callbacks(errorState)};
      yaml::parseInPlace(tree, *buffer, errorState);

      if (auto const reloaded = _session.reload(tree.rootref()); !reloaded)
      {
        return std::unexpected{reloaded.error()};
      }

      return _session.theme();
    }
    catch (ao::Exception const& error)
    {
      return makeError(Error::Code::FormatRejected, error.what());
    }
    catch (std::exception const& error)
    {
      return makeError(Error::Code::FormatRejected, formatResource("ThemeLoadFailedFormat", error.what()));
    }
  }
} // namespace ao::winui
