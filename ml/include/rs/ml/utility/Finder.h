/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <filesystem>
#include <boost/iterator/filter_iterator.hpp>
#include <set>

namespace rs::ml
{
  class Finder
  {
  public:
    Finder(const std::string& rootPath, const std::vector<std::string>& extensions)
      : _rootPath{rootPath},
        _extensions{extensions.begin(), extensions.end()}
    {
      _filter = [this](const std::filesystem::path& path)
      {
        return _extensions.find(path.extension().string()) != _extensions.end();
      };
    }

    auto begin() const
    {
      return boost::make_filter_iterator(_filter, Iterator{_rootPath});
    }

    auto end() const
    {
      return boost::make_filter_iterator(_filter, Iterator{});
    }

  private:
    using Iterator = std::filesystem::recursive_directory_iterator;

    std::string _rootPath;
    std::set<std::string> _extensions;
    std::function<bool(const std::filesystem::path&)> _filter;
  };

}

