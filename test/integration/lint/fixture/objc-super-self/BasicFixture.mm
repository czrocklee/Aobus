// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

__attribute__((objc_root_class))
@interface NSObject
- (instancetype)init;
- (instancetype)self;
@end

@interface AobusInitializerFixture : NSObject
@end

@implementation AobusInitializerFixture
- (instancetype)init
{
  // POSITIVE: FIX-TO: self = [super init];
  self = [super self];
  return self;
}
- (instancetype)initWithValidSuper
{
  self = [super init]; // NEGATIVE
  return self;
}
@end
