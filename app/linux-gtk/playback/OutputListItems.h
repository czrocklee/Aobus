// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>

#include <string>
#include <utility>

namespace ao::gtk
{
  /**
   * @brief GObject wrapper for a backend section header.
   */
  class BackendItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<BackendItem> create(audio::BackendId id, std::string name)
    {
      return Glib::make_refptr_for_instance<BackendItem>(new BackendItem{std::move(id), std::move(name)}); // NOLINT(cppcoreguidelines-owning-memory)
    }

    audio::BackendId const& id() const { return _id; }
    std::string const& name() const { return _name; }

  protected:
    BackendItem(audio::BackendId id, std::string name)
      : Glib::ObjectBase{typeid(BackendItem)}, _id{std::move(id)}, _name{std::move(name)}
    {
    }

  private:
    audio::BackendId _id;
    std::string _name;
  };

  /**
   * @brief GObject wrapper for an individual audio device/profile combination.
   */
  class DeviceItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<DeviceItem> create(audio::BackendId backend,
                                           audio::Device const& device,
                                           audio::ProfileId profile,
                                           std::string customName = "")
    {
      return Glib::make_refptr_for_instance<DeviceItem>(new DeviceItem(std::move(backend), device, std::move(profile), std::move(customName))); // NOLINT(cppcoreguidelines-owning-memory)
    }

    audio::BackendId const& backendId() const { return _backendId; }
    audio::ProfileId const& profileId() const { return _profileId; }
    audio::DeviceId const& id() const { return _id; }
    std::string const& name() const { return _name; }
    std::string const& description() const { return _description; }
    bool active() const { return _active; }
    void setActive(bool active) { _active = active; }

    // Helper for diffing
    bool matches(audio::BackendId const& backend, audio::DeviceId const& devId, audio::ProfileId const& profile) const
    {
      return _backendId == backend && _id == devId && _profileId == profile;
    }

  protected:
    DeviceItem(audio::BackendId backend,
               audio::Device const& device,
               audio::ProfileId profile,
               std::string customName)
      : Glib::ObjectBase{typeid(DeviceItem)}
      , _backendId{std::move(backend)}
      , _profileId{std::move(profile)}
      , _id{device.id}
      , _name{customName.empty() ? device.displayName : customName}
      , _description{device.description}
    {
    }

  private:
    audio::BackendId _backendId;
    audio::ProfileId _profileId;
    audio::DeviceId _id;
    std::string _name;
    std::string _description;
    bool _active = false;
  };
} // namespace ao::gtk
