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
    static Glib::RefPtr<BackendItem> create(ao::audio::BackendKind kind, std::string const& name)
    {
      return Glib::make_refptr_for_instance<BackendItem>(new BackendItem(kind, name));
    }

    ao::audio::BackendKind kind;
    std::string name;

  protected:
    BackendItem(ao::audio::BackendKind kind, std::string const& name)
      : Glib::ObjectBase(typeid(BackendItem)), kind(kind), name(name)
    {
    }
  };

  /**
   * @brief GObject wrapper for an individual audio device.
   */
  class DeviceItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<DeviceItem> create(ao::audio::BackendKind kind, ao::audio::Device const& device)
    {
      return Glib::make_refptr_for_instance<DeviceItem>(new DeviceItem(kind, device));
    }

    ao::audio::BackendKind kind;
    std::string id;
    std::string name;
    std::string description;
    bool active = false;

    // Helper for diffing
    bool matches(ao::audio::BackendKind k, std::string const& devId) const { return kind == k && id == devId; }

  protected:
    DeviceItem(ao::audio::BackendKind kind, ao::audio::Device const& device)
      : Glib::ObjectBase(typeid(DeviceItem))
      , kind(kind)
      , id(device.id)
      , name(device.displayName)
      , description(device.description)
    {
    }
  };
} // namespace ao::gtk
