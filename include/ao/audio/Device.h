// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>
#include <ao/utility/StrongType.h>

#include <string>

namespace ao::audio
{
  using DeviceId = utility::StrongType<std::string, struct DeviceTag>;

  struct Device final
  {
    DeviceId id{};
    std::string displayName{};
    std::string description{};
    bool isDefault = false;
    BackendId backendId{};
    bool operator==(Device const&) const = default;
  };
} // namespace ao::audio
