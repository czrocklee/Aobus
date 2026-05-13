// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>

namespace ao::gtk
{
  /**
   * @brief GObject wrapper for a backend section header.
   */
  class BackendItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<BackendItem> create(audio::BackendId const& id, std::string const& name)
    {
      return Glib::make_refptr_for_instance<BackendItem>(new BackendItem{id, name});
    }

    audio::BackendId id;
    std::string name;

  protected:
    BackendItem(audio::BackendId const& id, std::string const& name)
      : Glib::ObjectBase(typeid(BackendItem)), id(id), name(name)
    {
    }
  };

  /**
   * @brief GObject wrapper for an individual audio device/profile combination.
   */
  class DeviceItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<DeviceItem> create(audio::BackendId const& backend,
                                           audio::Device const& device,
                                           audio::ProfileId const& profile,
                                           std::string const& customName = "")
    {
      return Glib::make_refptr_for_instance<DeviceItem>(new DeviceItem(backend, device, profile, customName));
    }

    audio::BackendId backendId;
    audio::ProfileId profileId;
    audio::DeviceId id;
    std::string name;
    std::string description;
    bool active = false;

    // Helper for diffing
    bool matches(audio::BackendId const& b, audio::DeviceId const& devId, audio::ProfileId const& p) const
    {
      return backendId == b && id == devId && profileId == p;
    }

  protected:
    DeviceItem(audio::BackendId const& backend,
               audio::Device const& device,
               audio::ProfileId const& profile,
               std::string const& customName)
      : Glib::ObjectBase(typeid(DeviceItem))
      , backendId(backend)
      , profileId(profile)
      , id(device.id)
      , name(customName.empty() ? device.displayName : customName)
      , description(device.description)
    {
    }
  };
} // namespace ao::gtk
