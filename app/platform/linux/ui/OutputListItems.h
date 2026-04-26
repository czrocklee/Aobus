// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"
#include <glibmm/object.h>
#include <glibmm/refptr.h>

namespace app::ui
{

  /**
   * @brief GObject wrapper for a backend section header.
   */
  class BackendItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<BackendItem> create(app::core::playback::BackendKind kind, std::string const& name)
    {
      return Glib::make_refptr_for_instance<BackendItem>(new BackendItem(kind, name));
    }

    app::core::playback::BackendKind kind;
    std::string name;

  protected:
    BackendItem(app::core::playback::BackendKind kind, std::string const& name)
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
    static Glib::RefPtr<DeviceItem> create(app::core::playback::BackendKind kind,
                                           app::core::playback::AudioDevice const& device)
    {
      return Glib::make_refptr_for_instance<DeviceItem>(new DeviceItem(kind, device));
    }

    app::core::playback::BackendKind kind;
    std::string id;
    std::string name;
    std::string description;
    bool active = false;

    // Helper for diffing
    bool matches(app::core::playback::BackendKind k, std::string const& devId) const
    {
      return kind == k && id == devId;
    }

  protected:
    DeviceItem(app::core::playback::BackendKind kind, app::core::playback::AudioDevice const& device)
      : Glib::ObjectBase(typeid(DeviceItem))
      , kind(kind)
      , id(device.id)
      , name(device.displayName)
      , description(device.description)
    {
    }
  };

} // namespace app::ui
