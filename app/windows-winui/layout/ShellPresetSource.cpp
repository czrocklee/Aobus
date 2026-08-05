// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/ShellPresetSource.h"

#include "pch.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/utility/Path.h>
#include <ao/winui/layout/ShellDocument.h>

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

namespace ao::winui::layout
{
  namespace
  {
    /// Directory the running executable was loaded from.
    Result<std::filesystem::path> executableFolder()
    {
      // MAX_PATH is not the limit on a long-path-aware process, so the buffer
      // grows until the name fits rather than being assumed large enough.
      auto buffer = std::wstring(MAX_PATH, L'\0');

      while (true)
      {
        auto const written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (written == 0)
        {
          return makeError(Error::Code::NotFound,
                           std::format("The running executable could not be located (error {})", ::GetLastError()));
        }

        if (written < buffer.size())
        {
          buffer.resize(written);
          return std::filesystem::path{buffer}.parent_path();
        }

        buffer.resize(buffer.size() * 2);
      }
    }
  } // namespace

  Result<std::string> readShellPreset(ShellPreset const preset)
  {
    auto const resource = shellPresetResource(preset);
    auto folderRes = executableFolder();

    if (!folderRes)
    {
      return std::unexpected{folderRes.error()};
    }

    auto const path = *folderRes / kShellPresetFolder / resource;
    auto stream = std::ifstream{path, std::ios::binary};

    if (!stream)
    {
      return makeError(
        Error::Code::NotFound,
        std::format(
          "The shipped shell document '{}' is missing from {}", resource, utility::pathToUtf8(path.parent_path())));
    }

    auto yaml = std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};

    if (stream.bad())
    {
      return makeError(
        Error::Code::IoError, std::format("The shipped shell document '{}' could not be read", resource));
    }

    return yaml;
  }

  Result<uimodel::PreparedLayout> prepareShellPreset(ShellPreset const preset)
  {
    auto yamlRes = readShellPreset(preset);

    if (!yamlRes)
    {
      return std::unexpected{yamlRes.error()};
    }

    return prepareShellPresetDocument(*yamlRes, shellPresetResource(preset));
  }
} // namespace ao::winui::layout
