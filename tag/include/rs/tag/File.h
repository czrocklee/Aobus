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

#include <rs/tag/Metadata.h>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <filesystem>

namespace rs::tag
{
  class File
  {
  public:
    enum class Mode
    {
      ReadOnly,
      ReadWrite
    };

    File(const std::filesystem::path& path, Mode mode);

    virtual ~File() = default;

    virtual const Metadata loadMetadata() const = 0;

    virtual void saveMetadata(const Metadata& metadata) = 0;

  protected:
    boost::interprocess::file_mapping _fileMapping;
    boost::interprocess::mapped_region _mappedRegion;
  };
}