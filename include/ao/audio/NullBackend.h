// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/IBackend.h>

namespace ao::audio
{
  /**
   * @brief A backend that does nothing (used when no hardware is selected).
   *
   * Not marked final — TestBackend (in PlayerTest.cpp) inherits from it.
   */
  class NullBackend : public IBackend
  {
  public:
    NullBackend() = default;
    ~NullBackend() override = default;

    ao::Result<> open(Format const& /*format*/, RenderCallbacks /*callbacks*/) override { return {}; }
    void start() override {}
    void pause() override {}
    void resume() override {}
    void flush() override {}
    void stop() override {}
    void close() override {}

    Result<> setProperty(PropertyId id, PropertyValue const& value) override
    {
      if (id == PropertyId::Volume)
      {
        _volume = std::get<float>(value);
        return {};
      }

      if (id == PropertyId::Muted)
      {
        _muted = std::get<bool>(value);
        return {};
      }

      return std::unexpected(Error{.code = Error::Code::NotSupported});
    }

    Result<PropertyValue> getProperty(PropertyId id) const override
    {
      if (id == PropertyId::Volume)
      {
        return _volume;
      }

      if (id == PropertyId::Muted)
      {
        return _muted;
      }
      return std::unexpected(Error{.code = Error::Code::NotSupported});
    }

    PropertyInfo queryProperty(PropertyId id) const noexcept override
    {
      if (id == PropertyId::Volume || id == PropertyId::Muted)
      {
        return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
      }

      return {};
    }

    BackendId backendId() const noexcept override { return BackendId{kBackendNone}; }
    ProfileId profileId() const noexcept override { return ProfileId{kProfileShared}; }

  private:
    float _volume = 1.0f;
    bool _muted = false;
  };
} // namespace ao::audio
