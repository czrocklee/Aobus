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

#include <rs/ml/fbs/Track_generated.h>
#include <taglib/fileref.h>
#include <taglib/tbytevector.h>

#include <boost/asio/buffer.hpp>

namespace rs::ml::file
{
  class MusicFile
  {
  public:
    explicit MusicFile(const std::string& path) : _fileRef(path.c_str()){};
    fbs::TrackT loadTrack() const;
    TagLib::ByteVector loadAlbumArt() const;

  private:
    TagLib::FileRef _fileRef;
  };

}
