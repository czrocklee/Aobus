// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <rs/audio/BackendTypes.h>

namespace app::ui
{
  /**
   * @brief GObject wrapper for a backend section header.
   */
  class BackendItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<BackendItem> create(rs::audio::BackendKind kind, std::string const& name)
    {
      return Glib::make_refptr_for_instance<BackendItem>(new BackendItem(kind, name));
    }

    rs::audio::BackendKind kind;
    std::string name;

  protected:
    BackendItem(rs::audio::BackendKind kind, std::string const& name)
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
    static Glib::RefPtr<DeviceItem> create(rs::audio::BackendKind kind, rs::audio::AudioDevice const& device)
    {
      return Glib::make_refptr_for_instance<DeviceItem>(new DeviceItem(kind, device));
    }

    rs::audio::BackendKind kind;
    std::string id;
    std::string name;
    std::string description;
    bool active = false;

    // Helper for diffing
    bool matches(rs::audio::BackendKind k, std::string const& devId) const { return kind == k && id == devId; }

  protected:
    DeviceItem(rs::audio::BackendKind kind, rs::audio::AudioDevice const& device)
      : Glib::ObjectBase(typeid(DeviceItem))
      , kind(kind)
      , id(device.id)
      , name(device.displayName)
      , description(device.description)
    {
    }
  };
} // namespace app::ui
