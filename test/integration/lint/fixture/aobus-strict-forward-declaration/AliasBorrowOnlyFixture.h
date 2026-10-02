// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AliasTypes.h"

class TestAliasProviderBorrowOnly
{
  // POSITIVE: FIX-TO: void consume(/* forward declare */ CoResidentAliasOwner const& owner);
  void consume(CoResidentAliasOwner const& owner);
};
