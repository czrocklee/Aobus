// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ConfigStore.h"
#include <ao/utility/Log.h>

#include <fstream>

namespace ao::app
{
  ConfigStore::ConfigStore(std::filesystem::path filePath)
    : _filePath{std::move(filePath)}
  {
  }

  void ConfigStore::flush()
  {
    APP_LOG_INFO("Saving config to: {}", _filePath.string());
    std::filesystem::create_directories(_filePath.parent_path());
    auto file = std::ofstream{_filePath};
    file << _root;
  }

  void ConfigStore::ensureLoaded()
  {
    if (_loaded)
    {
      return;
    }

    _loaded = true;

    if (std::filesystem::exists(_filePath))
    {
      _root = YAML::LoadFile(_filePath.string());
    }
  }
}
