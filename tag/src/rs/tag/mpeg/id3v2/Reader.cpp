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

#include "Reader.h"
#include "Layout.h"
#include "Frame.h"
//#include ""

#include <map>
#include <memory>
#include <iostream>

namespace rs::tag::mpeg::id3v2
{
  namespace
  {
    template<MetaField Field, typename TextFrameViewT>
    struct TextFieldSetter
    {
      template<typename FrameViewT>
      void operator()(Metadata& meta, FrameViewT view)
      {
        std::cout << TextFrameViewT{view.data(), view.size()}.text() << std::endl;
        meta.set(Field, TextFrameViewT{view.data(), view.size()}.text());
      }
    };


    /*     std::map<std::string, std::function<void(Metadata&, const Atom&)>, std::less<>> MetadataSetters = {
        {TrknAtomLayout::Type,
          [](auto& meta, const auto& atom) {
            const auto& trkn = static_cast<const AtomView&>(atom).layout<TrknAtomLayout>();
            meta.set(MetaField::TrackNumber, static_cast<std::uint64_t>(trkn.trackNumber.value()));
            meta.set(MetaField::TotalTracks, static_cast<std::uint64_t>(trkn.totalTracks.value()));
          }}
        }; */

    Metadata loadV22Frames(const void* buffer, std::size_t size)
    {
      Metadata metadata;

      FrameViewIterator<V22FrameView> iter{buffer, size};
      FrameViewIterator<V22FrameView> end{};

      for (; iter != end; ++iter)
      {
        //const auto* common = reinterpret_cast<const V22FrameCommonLayout*>(buffer);
      }
    }
  }

  namespace
  {
    std::map<std::string, std::function<void(Metadata&, V23FrameView)>, std::less<>> MetadataSetters = {
     /*  {, [](auto& meta, const auto& atom) {
         const auto& trkn = static_cast<const AtomView&>(atom).layout<TrknAtomLayout>();
         meta.set(MetaField::TrackNumber, static_cast<std::uint64_t>(trkn.trackNumber.value()));
         meta.set(MetaField::TotalTracks, static_cast<std::uint64_t>(trkn.totalTracks.value()));
       }} */
       
      {"TIT2", TextFieldSetter<MetaField::Title, V23TextFrameView>{}},
      {"TALB", TextFieldSetter<MetaField::Album, V23TextFrameView>{}}
    };

    Metadata loadV23Frames(const void* buffer, std::size_t size)
    {
      Metadata metadata;

      FrameViewIterator<V23FrameView> frameIter{buffer, size};
      FrameViewIterator<V23FrameView> frameEnd{};

      for (; frameIter != frameEnd; ++frameIter)
      {
        std::cout << frameIter->id() << '/' << frameIter->size() << std::endl;

        if (auto setterIter = MetadataSetters.find(frameIter->id()); setterIter != MetadataSetters.end())
        {
          std::invoke(setterIter->second, metadata, *frameIter);
        }
    
      }

      return metadata;
    }
  }

  namespace
  {
    Metadata loadV24(const void* data, std::size_t size)
    {
      while (size > 0)
      {
      }
    }
  }

  Metadata loadFrames(const HeaderLayout& header, const void* buffer, std::size_t size)
  {
    switch (header.majorVersion)
    {
      case 2: return loadV22Frames(buffer, size);
      case 3: return loadV23Frames(buffer, size);
    }
  }
}