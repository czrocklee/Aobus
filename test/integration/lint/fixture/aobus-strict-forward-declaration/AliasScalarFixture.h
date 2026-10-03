// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AliasTypes.h"

class TestScalarAliasCoResident
{
  CoResidentScalarId _id;

  // NEGATIVE A scalar alias also requires its provider, not just its underlying type.
  void consume(CoResidentAliasOwner const& owner);
};
