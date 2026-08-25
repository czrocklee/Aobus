// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "CoreFoundationOwnership.h"

#include <ao/Error.h>

#include <CoreFoundation/CoreFoundation.h>

#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  Result<CoreFoundationPtr<::CFStringRef>> coreFoundationString(std::string_view utf8);

  Result<std::string> utf8String(::CFStringRef value);
} // namespace ao::audio::backend::detail
