// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "runtime/ConfigYamlTraits.h" // IWYU pragma: export

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ao::rt
{
  class ConfigStore final
  {
  public:
    enum class OpenMode : std::uint8_t
    {
      ReadWrite, // file may not exist yet, will be created on flush
      ReadOnly,  // file must already exist, NotFound is an error for load()
    };

    ~ConfigStore() = default;

    ConfigStore(ConfigStore const&) = delete;
    ConfigStore& operator=(ConfigStore const&) = delete;
    ConfigStore(ConfigStore&&) = delete;
    ConfigStore& operator=(ConfigStore&&) = delete;

    explicit ConfigStore(std::filesystem::path filePath, OpenMode mode = OpenMode::ReadWrite);

    Result<> flush();

    template<typename T>
    void save(std::string_view group, T const& obj)
    {
      if (_mode == OpenMode::ReadOnly)
      {
        throw std::logic_error{"save() called on ReadOnly ConfigStore"};
      }

      if (auto const result = ensureLoaded(); !result && result.error().code != Error::Code::NotFound)
      {
        return;
      }

      _root[std::string{group}] = YAML::Node{obj};
    }

    template<typename T>
    Result<> load(std::string_view group, T& obj)
    {
      if (auto const result = ensureLoaded(); !result)
      {
        return result;
      }

      if (auto const child = _root[std::string{group}])
      {
        try
        {
          obj = child.as<T>();
        }
        catch (std::exception const& e)
        {
          return makeError(
            Error::Code::FormatRejected, std::format("Failed to decode config key '{}': {}", group, e.what()));
        }
      }

      return {};
    }

  private:
    Result<> ensureLoaded();

    std::filesystem::path _filePath;
    OpenMode _mode = OpenMode::ReadWrite;
    YAML::Node _root;
    bool _loaded = false;
  };
}
