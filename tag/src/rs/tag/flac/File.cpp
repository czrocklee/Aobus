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

#include <rs/tag/flac/File.h>
#include <rs/common/Exception.h>
#include <boost/asio/buffer.hpp>
#include <boost/endian/conversion.hpp>
#include <iostream>
#include <map>
#include <charconv>
#include "MetadataBlock.h"

namespace rs::tag::flac
{
  namespace
  {
    class MetadataBlock
    {

    };
    
    
  }

    

  const Metadata File::loadMetadata() const
  {
    if (_mappedRegion.get_size() < 4 || std::memcmp(_mappedRegion.get_address(), "fLaC", 4) != 0)
    {
      RS_THROW(common::Exception, "unrecognized flac file content");
    }

    MetadataBlockViewIterator iter{static_cast<const char*>(_mappedRegion.get_address()) + 4, _mappedRegion.get_size() - 4};
    MetadataBlockViewIterator end{};

    for (; iter != end; ++iter)
    {
      std::cout << " type " << static_cast<int>(iter->type()) << " size " << iter->size() << std::endl; 

      if (iter->type() == MetadataBlockType::VorbisComment)
      {
        VorbisCommentBlockView block{iter->data()};

        for (auto strView : block.comments())
        {
          std::cout << '\t' << strView << std::endl;
        }
      }
    }

    return {};
  }

  void File::saveMetadata(const Metadata& metadata) { }
}