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

#include <rs/tag/File.h>

namespace rs::tag
{
  namespace
  {
    boost::interprocess::mode_t fromMode(File::Mode mode)
    {
      return mode == File::Mode::ReadOnly ? boost::interprocess::read_only : boost::interprocess::read_write;
    }
  }

  File::File(const std::filesystem::path& path, Mode mode)
    : _fileMapping{path.c_str(), fromMode(mode)},
      _mappedRegion{_fileMapping, fromMode(mode)}
  {}
}