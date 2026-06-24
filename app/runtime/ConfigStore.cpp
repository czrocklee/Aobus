// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/utility/AtomicFile.h>
#include <ao/yaml/Utils.h>

#include <exception>
#include <filesystem>
#include <format>
#include <string>
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
      throwException<Exception>("flush() called on ReadOnly ConfigStore");
    }

    APP_LOG_INFO("Saving config to: {}", _filePath.string());

    auto const yaml = ryml::emitrs_yaml<std::string>(_root);
    return utility::writeAtomically(_filePath, yaml, utility::AtomicFilePermissions::OwnerReadWrite);
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

      _root.to_map(0);
      return {};
    }

    try
    {
      auto const fileName = _filePath.string();
      _inputBuffer = yaml::readFile(_filePath);
      _root = ryml::Tree{yaml::callbacks(fileName.c_str())};
      ryml::parse_in_place(yaml::toSubstr(_inputBuffer), &_root);
    }
    catch (std::exception const& e)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to parse config file '{}': {}", _filePath.string(), e.what()));
    }

    return {};
  }
} // namespace ao::rt
