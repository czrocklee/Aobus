// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TemplateValueTypes.h"

using CoResidentValueId = AliasValueWrapper<int>;
typedef AliasValueWrapper<long> CoResidentLegacyId;
using CoResidentScalarId = int;
template<typename T>
using CoResidentTemplateId = AliasValueWrapper<T>;

class CoResidentAliasOwner
{
  // An alias used only inside this provider must not make the provider required.
  CoResidentValueId _id;
};
