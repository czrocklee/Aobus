// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ObjCIvarDeclarations.h"

#include <cstdint>

extern "C" void consume_integer(long long*);
extern "C" void consume_integer_value(long long);
extern "C" void consume_address(std::uintptr_t);

@implementation AobusImportedIvarFixture
@end

@protocol AobusObjCParameterSink
- (void)setRow:(long long)row; // NEGATIVE
@end

__attribute__((objc_root_class))
@interface AobusObjCIvarFixture<AobusObjCParameterSink> {
  long long inheritedValue; // NEGATIVE
@protected
  long long protectedValue; // NEGATIVE
@public
  long long publicValue; // NEGATIVE
@package
  long long packageValue; // NEGATIVE
@private
  // POSITIVE
  long unrelatedLong;
  // POSITIVE: FIX-TO: std::int32_t count;
  int count;
  long long externalValue; // NEGATIVE
  long long categoryValue; // NEGATIVE
  long long castValue;     // NEGATIVE
}
- (void)send;
- (void)setRow:(long long)row;     // NEGATIVE
- (void)consumeRow:(long long)row; // NEGATIVE
+ (void)setColumn:(int)column;     // NEGATIVE
@end

@implementation AobusObjCIvarFixture
- (void)setRow:(long long)row // NEGATIVE
{
  // POSITIVE: FIX-TO: std::int32_t localRow = 0;
  int localRow = 0;
}
- (void)consumeRow:(long long)row // NEGATIVE
{
  consume_integer(&row);
}
+ (void)setColumn:(int)column // NEGATIVE
{
}
- (void)send
{
  consume_integer(&externalValue);
  consume_integer(reinterpret_cast<long long*>(&castValue));
  long long localValue = 0; // NEGATIVE
  consume_integer(&localValue);
  // POSITIVE: FIX-TO: std::int32_t localCount = 0;
  int localCount = 0;
}
@end

@interface AobusObjCIvarFixture (External)
- (void)sendCategoryValue;
@end

@implementation AobusObjCIvarFixture (External)
- (void)sendCategoryValue
{
  consume_integer(&categoryValue);
}
@end

@interface AobusObjCParameterChild : AobusObjCIvarFixture
- (void)setRow:(long long)row; // NEGATIVE
@end

@implementation AobusObjCParameterChild
- (void)setRow:(long long)row // NEGATIVE
{
}
@end

void inspectBlockParameters()
{
  auto externalBlock = ^(long long value) { // NEGATIVE
    consume_integer(&value);
    long long localValue = 0; // NEGATIVE
    consume_integer(&localValue);
  };
  // POSITIVE: FIX-TO: auto ordinaryBlock = ^(std::int32_t value) {
  auto ordinaryBlock = ^(int value) {
    // POSITIVE: FIX-TO: std::int32_t localCount = 0;
    int localCount = 0;
  };
  auto externalLambda = [](long long value) { // NEGATIVE
    consume_integer(&value);
  };
}

void inspectExplicitBoundaryCasts()
{
  long long cStyleValue = 0; // NEGATIVE
  consume_integer((long long*)&cStyleValue);
  long long reinterpretValue = 0; // NEGATIVE
  consume_integer(reinterpret_cast<long long*>(&reinterpretValue));
  long long referenceValue = 0; // NEGATIVE
  consume_integer(&reinterpret_cast<long long&>(referenceValue));
  long long encodedValue = 0; // NEGATIVE
  consume_address(reinterpret_cast<std::uintptr_t>(&encodedValue));
  long long wrappedAddress = 0; // NEGATIVE
  consume_address(static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(&wrappedAddress)));
  long long convertedValue = 0; // NEGATIVE
  // POSITIVE: FIX-TO: consume_integer_value(static_cast<std::int64_t>(convertedValue));
  consume_integer_value(static_cast<long long>(convertedValue));
}

__attribute__((objc_root_class))
@interface AobusDeclarationOnlyFixture {
  long long unavailableImplementation; // NEGATIVE
}
@end
