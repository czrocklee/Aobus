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

#include <rs/ml/core/DynamicList.H>
#include <rs/ml/core/Track.H>
#include <rs/ml/core/UpdateObserverable.H>
#include <boost/container/flat_map.hpp>
#include <iostream>

namespace rs::ml::core
{
  using Writer = MediaLibrary::Writer;
  using Reader = MediaLibrary::Reader;

  struct DynamicList::Impl : public MediaLibrary::Observer
  {
    MediaLibrary& ml;
    Filter filter;
    boost::container::flat_map<TrackId, std::unique_ptr<TrackT>> tracks;
    UpdateObserverable<std::size_t> observerable;

    Impl(MediaLibrary& ml, Filter&& filter): ml{ml}, filter{std::move(filter)} { ml.attach(*this); }
    ~Impl() { ml.detach(*this); }
    void onAttached() override;
    void onCreate(TrackId id, const Track* track) override;  
    void onModify(TrackId id, const Track* track) override;
    void onRemove(TrackId id, const Track* track) override;
  };

  void DynamicList::Impl::onAttached()
  {
    for (auto pair : ml.reader())
    {
      if (!filter || filter(pair.second))
      {
        //tracks.emplace_hint(tracks.end(), pair.first, pair.second->UnPack());
        tracks.emplace_hint(tracks.end(), pair.first, nullptr);
      }
    }
  }

  void DynamicList::Impl::onCreate(TrackId id, const Track* track)
  { 
    if (!filter || filter(track))
    {
      assert(id > (tracks.empty() ? InvalidTrackId : tracks.rbegin()->first));
      //auto iter = tracks.emplace_hint(tracks.end(), id, track->UnPack());
      auto iter = tracks.emplace_hint(tracks.end(), id, nullptr);
      observerable.create(tracks.index_of(iter));
    }
  }

  void DynamicList::Impl::onModify(TrackId id, const Track* track)
  { 
    auto iter = tracks.find(id);

    if (!filter || filter(track))
    {
      if (iter == tracks.end())
      {
        iter = tracks.emplace(id, track->UnPack()).first;
        observerable.create(tracks.index_of(iter));
      }
      else
      {
        iter->second.reset(track->UnPack());
        observerable.modify(tracks.index_of(iter));
      }
    }
    else if (iter != tracks.end())
    {
      std::size_t index = tracks.index_of(iter);
      observerable.remove(index);
      tracks.erase(iter);
    }
  }

  void DynamicList::Impl::onRemove(TrackId id, const Track*)
  { 
    auto iter = tracks.find(id);

    if (iter != tracks.end())
    {
      std::size_t index = tracks.index_of(iter);
      observerable.remove(index);
      tracks.erase(iter);
    }
  }

  DynamicList::DynamicList(MediaLibrary& ml, Filter filter)
    : _impl{std::make_unique<Impl>(ml, std::move(filter))}
  {
  }

  DynamicList::~DynamicList()
  {
  }

  std::size_t DynamicList::size() const
  {
    return _impl->tracks.size();
  }

  DynamicList::Value DynamicList::operator[](std::size_t index) const
  {
    auto iter = _impl->tracks.nth(index);
    
    if (!iter->second)
    {
      auto reader = _impl->ml.reader();
      iter->second.reset(reader[iter->first]->UnPack());
    }

    return {iter->first, *iter->second};
  }

  void DynamicList::attach(Observer& observer)
  {
    _impl->observerable.attach(observer);
  }

  void DynamicList::detach(Observer& observer)
  {
    _impl->observerable.detach(observer);
  }

}
