// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/library/LibraryChanges.h>

#include <string>
#include <utility>

namespace ao::rt::test
{
  struct LibraryChangesAccess final
  {
    static async::Subscription bindReplica(LibraryChanges const& changes,
                                           std::string replicaName,
                                           compat::MoveOnlyFunction<void(LibraryChangeSet const&)> apply)
    {
      return changes.bindReplica(std::move(replicaName), std::move(apply));
    }
  };
} // namespace ao::rt::test
