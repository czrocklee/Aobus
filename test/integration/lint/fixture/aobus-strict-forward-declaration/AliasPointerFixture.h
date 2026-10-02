// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AliasTypes.h"

class TestAliasPointerCoResident
{
  CoResidentValueId const* _id;
  void consumeId(CoResidentLegacyId const& id);

  // NEGATIVE Even a pointer/reference requires the alias declaration's provider.
  void consume(CoResidentAliasOwner const& owner);
};
