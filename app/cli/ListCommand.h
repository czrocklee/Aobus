// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/LibraryAuthoring.h>

namespace ao::cli
{
  Result<> validateListOrderCommandStatus(rt::AuthoringStatus status);
}
