// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

__attribute__((objc_root_class))
@interface AobusPropertyFixture
// POSITIVE
@property (nonatomic) int ItemCount;
@property (nonatomic) int rowCount; // NEGATIVE
@end
