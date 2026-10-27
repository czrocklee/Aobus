// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AliasTypes.h"
#include "WeakOnlyTypes.h"

class TestAliasTemplateCoResident
{
  CoResidentTemplateId<double> _id;

  // NEGATIVE The alias template requires the co-resident owner's provider.
  void consume(CoResidentAliasOwner const& owner);

  // POSITIVE
  void consumeIsolated(TargetBadRawPtr const& target);
};
