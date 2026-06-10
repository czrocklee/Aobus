// RUN: %check_clang_tidy %s aobus-implicit-bool-conversion-in-init %t

int scalar();
void* raw();

struct WithMember
{
  int member;
};

#define AOBUS_TRUTHY(x) x

void test()
{
  // POSITIVE: FIX-TO: if (auto ptr = raw(); ptr != nullptr)
  if (auto ptr = raw(); ptr)
  {
  }

  // POSITIVE: FIX-TO: if (auto value = scalar(); value != 0)
  if (auto value = scalar(); value)
  {
  }

  // POSITIVE: FIX-TO: if (auto memberPtr = &WithMember::member; memberPtr != nullptr)
  if (auto memberPtr = &WithMember::member; memberPtr)
  {
  }

  // POSITIVE - diagnostic fires; no FixIt because the variable is spelled inside a macro
  if (auto flag = scalar(); AOBUS_TRUTHY(flag))
  {
  }

  // NEGATIVE
  if (auto ptr = raw(); ptr != nullptr)
  {
  }

  // NEGATIVE
  if (auto value = scalar(); value != 0)
  {
  }

  // NEGATIVE
  if (auto ptr = raw(); !ptr)
  {
  }

  // NEGATIVE
  if (auto value = scalar(); !value)
  {
  }

  auto ptr2 = raw();
  // NEGATIVE (handled by standard check, not ours)
  if (ptr2)
  {
  }

  int value2 = scalar();
  // NEGATIVE (handled by standard check, not ours)
  if (value2)
  {
  }
}
