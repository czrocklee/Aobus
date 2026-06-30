// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>

#include <glibmm/object.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>

#include <string>

namespace ao::gtk
{
  /**
   * @brief GObject wrapper for a backend section header.
   */
  class OutputBackendItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<OutputBackendItem> create(audio::BackendId id, std::string name);

    audio::BackendId const& id() const;
    std::string const& name() const;

  protected:
    OutputBackendItem();

  private:
    audio::BackendId _id;
    std::string _name;
  };

  /**
   * @brief GObject wrapper for an individual audio device/profile combination.
   */
  class OutputDeviceItem final : public Glib::Object
  {
  public:
    static Glib::RefPtr<OutputDeviceItem> create(audio::BackendId backend,
                                                 audio::Device const& device,
                                                 audio::ProfileId profile,
                                                 std::string badge = "");

    audio::BackendId const& backendId() const;
    audio::ProfileId const& profileId() const;
    audio::DeviceId const& id() const;
    std::string const& name() const;
    std::string const& description() const;
    std::string const& badge() const;
    bool active() const;
    void setActive(bool active);

    // Helper for diffing
    bool matches(audio::BackendId const& backend, audio::DeviceId const& devId, audio::ProfileId const& profile) const;

  protected:
    OutputDeviceItem();

  private:
    audio::BackendId _backendId;
    audio::ProfileId _profileId;
    audio::DeviceId _id;
    std::string _name;
    std::string _description;
    std::string _badge;
    bool _active = false;
  };
} // namespace ao::gtk
