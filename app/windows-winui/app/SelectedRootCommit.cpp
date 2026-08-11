// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/SelectedRootCommit.h>

#include <ao/Error.h>
#include <ao/utility/Path.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <filesystem>
#include <format>

namespace ao::winui
{
  Result<DesktopSettings> prepareSelectedRootCommit(DesktopSettings const& settings, std::filesystem::path const& root)
  {
    auto candidate = settings;

    try
    {
      candidate.lastLibraryPath = utility::pathToUtf8(root);
    }
    catch (std::filesystem::filesystem_error const& error)
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Failed to encode the selected library path: {}", error.what()));
    }

    return candidate;
  }
} // namespace ao::winui
