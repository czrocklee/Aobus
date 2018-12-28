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

#include <rs/ml/core/Track.h>
#include <rs/ml/core/UpdateObserverable.h>
#include <boost/container/flat_map.hpp>
#include <functional>
#include <type_traits>

namespace rs::ml::core
{
  template<typename Track, typename Transform>
  struct DynamicTrackListImpl
  {
  public:
    using Filter = std::function<bool(Track)>;

    DynamicTrackListImpl(Filter&& filter) : _filter{std::move(filter)} {}

    std::size_t size() const { return _tracks.size(); }
    std::pair<TrackId, const TrackT&> get(std::size_t index) const;

    using Observer = UpdateObserver<TrackId, const TrackT&, std::size_t>;
    void attach(Observer& observer) { _observerable.attach(observer); }
    void detach(Observer& observer) { _observerable.detach(observer); }

  protected:
    using TrackPtr = std::invoke_result_t<Transform, Track>;
    using Container = boost::container::flat_map<TrackId, TrackPtr>;
    using Iterator = typename Container::iterator;

    void onCreate(TrackId id, Track track);
    void onModify(TrackId id, Track track);
    void onRemove(TrackId id);

    auto create(TrackId id, TrackPtr track);
    void modify(Iterator iter, TrackPtr track);
    void remove(Iterator iter);

  private:
    Filter _filter;
    Transform _trans;
    Container _tracks;
    UpdateObserverable<TrackId, const TrackT&, std::size_t> _observerable;
  };

  template<typename Track, typename Transform>

  std::pair<TrackId, const TrackT&> DynamicTrackListImpl<Track, Transform>::get(std::size_t index) const
  {
    auto iter = _tracks.nth(index);
    return {iter->first, *iter->second};
  }

  template<typename Track, typename Transform>
  void DynamicTrackListImpl<Track, Transform>::onCreate(TrackId id, Track track)
  {
    if (!_filter || _filter(track))
    {
      create(id, _trans(track));
    }
  }

  template<typename Track, typename Transform>
  void DynamicTrackListImpl<Track, Transform>::onModify(TrackId id, Track track)
  {
    auto iter = _tracks.find(id);

    if (!_filter || _filter(track))
    {
      if (iter == _tracks.end())
      {
        create(id, _trans(track));
      }
      else
      {
        modify(iter, _trans(track));
      }
    }
    else if (iter != _tracks.end())
    {
      remove(iter);
    }
  }

  template<typename Track, typename Transform>
  void DynamicTrackListImpl<Track, Transform>::onRemove(TrackId id)
  {
    auto iter = _tracks.find(id);

    if (iter != _tracks.end())
    {
      remove(iter);
    }
  }

  template<typename Track, typename Transform>
  auto DynamicTrackListImpl<Track, Transform>::create(TrackId id, TrackPtr track)
  {
    assert(id > (_tracks.empty() ? InvalidTrackId : _tracks.rbegin()->first));
    auto iter = _tracks.emplace_hint(_tracks.end(), id, std::move(track));
    _observerable.create(id, *(iter->second), _tracks.index_of(iter));
    return iter;
  }

  template<typename Track, typename Transform>
  void DynamicTrackListImpl<Track, Transform>::modify(Iterator iter, TrackPtr track)
  {
    iter->second = std::move(track);
    _observerable.modify(iter->first, *(iter->second), _tracks.index_of(iter));
  }

  template<typename Track, typename Transform>
  void DynamicTrackListImpl<Track, Transform>::remove(Iterator iter)
  {
    std::size_t index = _tracks.index_of(iter);
    _observerable.remove(iter->first, *(iter->second), index);
    _tracks.erase(iter);
  }  
}
