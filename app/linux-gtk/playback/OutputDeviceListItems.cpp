// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDeviceListItems.h"

#include <ao/audio/Backend.h>

#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>

#include <string>
#include <utility>

namespace ao::gtk
{
  OutputBackendItem::OutputBackendItem()
    : Glib::ObjectBase{"OutputBackendItem"}
  {
  }

  Glib::RefPtr<OutputBackendItem> OutputBackendItem::create(audio::BackendId id, std::string name)
  {
    auto itemPtr = Glib::make_refptr_for_instance<OutputBackendItem>(new OutputBackendItem{});
    itemPtr->_id = std::move(id);
    itemPtr->_name = std::move(name);
    return itemPtr;
  }

  audio::BackendId const& OutputBackendItem::id() const
  {
    return _id;
  }

  std::string const& OutputBackendItem::name() const
  {
    return _name;
  }

  OutputDeviceItem::OutputDeviceItem()
    : Glib::ObjectBase{"OutputDeviceItem"}
  {
  }

  Glib::RefPtr<OutputDeviceItem> OutputDeviceItem::create(audio::BackendId backend,
                                                          audio::Device const& device,
                                                          audio::ProfileId profile,
                                                          std::string customName)
  {
    auto itemPtr = Glib::make_refptr_for_instance<OutputDeviceItem>(new OutputDeviceItem{});
    itemPtr->_backendId = std::move(backend);
    itemPtr->_profileId = std::move(profile);
    itemPtr->_id = device.id;
    itemPtr->_name = customName.empty() ? device.displayName : std::move(customName);
    itemPtr->_description = device.description;
    return itemPtr;
  }

  audio::BackendId const& OutputDeviceItem::backendId() const
  {
    return _backendId;
  }

  audio::ProfileId const& OutputDeviceItem::profileId() const
  {
    return _profileId;
  }

  audio::DeviceId const& OutputDeviceItem::id() const
  {
    return _id;
  }

  std::string const& OutputDeviceItem::name() const
  {
    return _name;
  }

  std::string const& OutputDeviceItem::description() const
  {
    return _description;
  }

  bool OutputDeviceItem::active() const
  {
    return _active;
  }

  void OutputDeviceItem::setActive(bool active)
  {
    _active = active;
  }

  bool OutputDeviceItem::matches(audio::BackendId const& backend,
                                 audio::DeviceId const& devId,
                                 audio::ProfileId const& profile) const
  {
    return _backendId == backend && _id == devId && _profileId == profile;
  }
} // namespace ao::gtk
