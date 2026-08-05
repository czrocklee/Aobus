// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "theme/ThemeCoordinator.h"

#include "platform/StringResources.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/utility/Path.h>
#include <ao/winui/Theme.h>
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

  ThemeCoordinator::ThemeCoordinator(std::filesystem::path themePath)
    : _themePath{std::move(themePath)}
  {
  }

  Result<winui::Theme> ThemeCoordinator::reload()
  {
    if (!std::filesystem::exists(_themePath))
    {
      return makeError(
        Error::Code::NotFound, formatResource("ThemeFileNotFoundFormat", utility::pathToUtf8(_themePath)));
    }

    auto bufferRes = yaml::readFileResult(_themePath, kMaximumThemeBytes);

    if (!bufferRes)
    {
      return std::unexpected{bufferRes.error()};
    }

    try
    {
      auto errorState = yaml::ErrorCallbackState{utility::pathToUtf8(_themePath)};
      auto tree = ryml::Tree{yaml::callbacks(errorState)};
      yaml::parseInPlace(tree, *bufferRes, errorState);

      if (auto const reloadedRes = _session.reload(tree.rootref()); !reloadedRes)
      {
        return std::unexpected{reloadedRes.error()};
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
