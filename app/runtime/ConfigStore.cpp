// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/utility/AtomicFile.h>
#include <ao/yaml/RymlAdapter.h>

#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <utility>

namespace ao::rt
{
  ConfigStore::ConfigStore(std::filesystem::path filePath, OpenMode mode)
    : _filePath{std::move(filePath)}, _yamlContext{_filePath.string()}, _mode{mode}
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

    auto fileEc = std::error_code{};
    auto const fileExists = std::filesystem::exists(_filePath, fileEc);

    if (fileEc)
    {
      return makeError(Error::Code::IoError,
                       std::format("Failed to inspect config file '{}': {}", _filePath.string(), fileEc.message()));
    }

    if (!fileExists)
    {
      if (_mode == OpenMode::ReadOnly)
      {
        return makeError(Error::Code::NotFound, std::format("Config file not found: {}", _filePath.string()));
      }

      _root.to_map(0);
      _loaded = true;
      return {};
    }

    auto bufferResult = yaml::readFileResult(_filePath);

    if (!bufferResult)
    {
      return std::unexpected{bufferResult.error()};
    }

    _inputBuffer = std::move(*bufferResult);

    try
    {
      _root = ryml::Tree{yaml::callbacks(_yamlContext)};
      yaml::parseInPlace(_root, _inputBuffer, _yamlContext);
    }
    catch (std::exception const& e)
    {
      return makeError(
        Error::Code::FormatRejected, std::format("Failed to parse config file '{}': {}", _filePath.string(), e.what()));
    }

    _loaded = true;
    return {};
  }
} // namespace ao::rt
