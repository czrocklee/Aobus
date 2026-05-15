// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ConfigStore.h"

#include <ao/Error.h>
#include <ao/utility/Log.h>

#include <yaml-cpp/node/parse.h>

#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <utility>

namespace ao::rt
{
  ConfigStore::ConfigStore(std::filesystem::path filePath, OpenMode mode)
    : _filePath{std::move(filePath)}, _mode{mode}
  {
  }

  Result<> ConfigStore::flush()
  {
    if (_mode == OpenMode::ReadOnly)
    {
      throw std::logic_error{"flush() called on ReadOnly ConfigStore"};
    }

    APP_LOG_INFO("Saving config to: {}", _filePath.string());
    std::filesystem::create_directories(_filePath.parent_path());
    auto file = std::ofstream{_filePath};
    file << _root;

    if (!file.good())
    {
      return makeError(Error::Code::IoError, std::format("Failed to write config file: {}", _filePath.string()));
    }

    return {};
  }

  Result<> ConfigStore::ensureLoaded()
  {
    if (_loaded)
    {
      return {};
    }

    _loaded = true;

    if (!std::filesystem::exists(_filePath))
    {
      if (_mode == OpenMode::ReadOnly)
      {
        return makeError(Error::Code::NotFound, std::format("Config file not found: {}", _filePath.string()));
      }

      return {};
    }

    try
    {
      _root = YAML::LoadFile(_filePath.string());
    }
    catch (std::exception const& e)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to parse config file '{}': {}", _filePath.string(), e.what()));
    }

    return {};
  }
}
