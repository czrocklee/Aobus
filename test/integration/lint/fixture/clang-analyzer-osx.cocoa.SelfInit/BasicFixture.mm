// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

__attribute__((objc_root_class))
@interface NSObject
- (instancetype)init;
@end

@interface AobusAnalyzerFixture : NSObject
@end

@implementation AobusAnalyzerFixture
- (instancetype)init
{
  [super init];
  return self; // POSITIVE
}
- (instancetype)initWithValidSuper
{
  self = [super init]; // NEGATIVE
  return self;         // NEGATIVE
}
@end
