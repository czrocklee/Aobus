// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/iterator/filter_iterator.hpp>
#include <filesystem>
#include <functional>
#include <set>

namespace rs::utility
{
  class Finder
  {
  public:
    Finder(std::string const& rootPath, std::vector<std::string> const& extensions)
      : _rootPath{rootPath}
      , _extensions{extensions.begin(), extensions.end()}
    {
      _filter = [this](std::filesystem::path const& path) {
        return _extensions.find(path.extension().string()) != _extensions.end();
      };
    }

    [[nodiscard]] auto begin() const { return boost::make_filter_iterator(_filter, Iterator{_rootPath}); }

    [[nodiscard]] auto end() const { return boost::make_filter_iterator(_filter, Iterator{}); }

  private:
    using Iterator = std::filesystem::recursive_directory_iterator;

    std::string _rootPath;
    std::set<std::string> _extensions;
    std::function<bool(std::filesystem::path const&)> _filter;
  };

}
