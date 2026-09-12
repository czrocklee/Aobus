// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

// This context is consumed by ObjCIvarFixture.mm; standalone header checks use C++.
#ifdef __OBJC__
__attribute__((objc_root_class))
@interface AobusImportedIvarFixture {
@private
  long long headerValue; // NEGATIVE
}
@end
#endif
