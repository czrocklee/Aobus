// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputListItems.h"

#include <ao/audio/Backend.h>

#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>

#include <string>
#include <utility>

namespace ao::gtk
{
  BackendItem::BackendItem()
    : Glib::ObjectBase{"BackendItem"}
  {
  }

  Glib::RefPtr<BackendItem> BackendItem::create(audio::BackendId id, std::string name)
  {
    auto itemPtr = Glib::make_refptr_for_instance<BackendItem>(new BackendItem{});
    itemPtr->_id = std::move(id);
    itemPtr->_name = std::move(name);
    return itemPtr;
  }

  audio::BackendId const& BackendItem::id() const
  {
    return _id;
  }

  std::string const& BackendItem::name() const
  {
    return _name;
  }

  DeviceItem::DeviceItem()
    : Glib::ObjectBase{"DeviceItem"}
  {
  }

  Glib::RefPtr<DeviceItem> DeviceItem::create(audio::BackendId backend,
                                              audio::Device const& device,
                                              audio::ProfileId profile,
                                              std::string customName)
  {
    auto itemPtr = Glib::make_refptr_for_instance<DeviceItem>(new DeviceItem{});
    itemPtr->_backendId = std::move(backend);
    itemPtr->_profileId = std::move(profile);
    itemPtr->_id = device.id;
    itemPtr->_name = customName.empty() ? device.displayName : std::move(customName);
    itemPtr->_description = device.description;
    return itemPtr;
  }

  audio::BackendId const& DeviceItem::backendId() const
  {
    return _backendId;
  }

  audio::ProfileId const& DeviceItem::profileId() const
  {
    return _profileId;
  }

  audio::DeviceId const& DeviceItem::id() const
  {
    return _id;
  }

  std::string const& DeviceItem::name() const
  {
    return _name;
  }

  std::string const& DeviceItem::description() const
  {
    return _description;
  }

  bool DeviceItem::active() const
  {
    return _active;
  }

  void DeviceItem::setActive(bool active)
  {
    _active = active;
  }

  bool DeviceItem::matches(audio::BackendId const& backend,
                           audio::DeviceId const& devId,
                           audio::ProfileId const& profile) const
  {
    return _backendId == backend && _id == devId && _profileId == profile;
  }
} // namespace ao::gtk
