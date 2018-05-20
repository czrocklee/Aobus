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

#include "DynamicTrackListImpl.H"
#include <rs/ml/core/MediaLibrary.H>
#include <rs/ml/core/TrackListZero.H>

#include <iostream>

namespace
{
  struct UnPack 
  { 
    using Ptr = std::unique_ptr<const rs::ml::core::TrackT>;
    Ptr operator()(const rs::ml::core::Track* track) { return Ptr{track->UnPack()}; } 
  };
}

namespace rs::ml::core
{
  struct TrackListZero::Impl : public DynamicTrackListImpl<const Track*, UnPack>, public MediaLibrary::Observer
  {
    using Base = DynamicTrackListImpl<const Track*, UnPack>;

  public:
    Impl(MediaLibrary& ml, Filter&& filter) : Base{std::move(filter)}, _ml{ml} { _ml.attach(*this); }
    ~Impl() { _ml.detach(*this);  std::cout << "impl destroied" << std::endl; }

    void mutate(std::size_t index, const TrackT& value);

    void onAttached() override;
    void onCreate(TrackId id, const Track* track) override;  
    void onModify(TrackId id, const Track* track) override;
    void onRemove(TrackId id, const Track* track) override;

  private:
    MediaLibrary& _ml;
  };

  void TrackListZero::Impl::mutate(std::size_t index, const TrackT& value)
  {
    TrackId id = get(index).first;
    auto writer = _ml.writer();
    writer.modify(id, value);
  }

  void TrackListZero::Impl::onAttached()
  {
    for (auto pair : _ml.reader())
    {
      Base::onCreate(pair.first, pair.second);
    }
  }

  void TrackListZero::Impl::onCreate(TrackId id, const Track* track)
  {
    Base::onCreate(id, track);
  }

  void TrackListZero::Impl::onModify(TrackId id, const Track* track)
  {
    Base::onModify(id, track);
  }

  void TrackListZero::Impl::onRemove(TrackId id, const Track*)
  { 
    Base::onRemove(id);
  }

  TrackListZero::TrackListZero(MediaLibrary& ml, Filter filter)
    : _impl{std::make_unique<Impl>(ml, std::move(filter))}
  {
  }

  TrackListZero::~TrackListZero()
  {
  }

  std::size_t TrackListZero::size() const
  {
    return _impl->size();
  }

  TrackListZero::Value TrackListZero::operator[](std::size_t index) const
  {
    return _impl->get(index);
  }

  void TrackListZero::mutate(std::size_t index, const TrackT& value)
  {
    _impl->mutate(index, value);
  }

  void TrackListZero::attach(Observer& observer)
  {
    _impl->attach(observer);
  }

  void TrackListZero::detach(Observer& observer)
  {
    _impl->detach(observer);
  }
}
