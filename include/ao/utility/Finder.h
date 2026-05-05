// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <filesystem>
#include <ranges>
#include <set>

namespace ao::utility
{
  class Finder
  {
  public:
    Finder(std::string rootPath, std::vector<std::string> extensions)
      : _rootPath{std::move(rootPath)}, _extensions{extensions.begin(), extensions.end()}
    {
    }

    auto paths() const
    {
      return std::filesystem::recursive_directory_iterator(_rootPath) |
             std::views::filter([exts = _extensions](auto const& entry)
                                { return exts.contains(entry.path().extension().string()); }) |
             std::views::transform([](auto const& entry) { return entry.path(); });
    }

  private:
    std::string _rootPath;
    std::set<std::string> _extensions;
  };
}
