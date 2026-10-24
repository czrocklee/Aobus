// RUN: %check_clang_tidy %s aobus-implicit-bool-conversion-in-init %t

int scalar();
double fractional();
void* raw();

struct WithMember
{
  int member;
};

#define AOBUS_TRUTHY(x) x

void test()
{
  // POSITIVE | use an explicit '!= nullptr' comparison instead
  if (auto ptr = raw(); ptr)
  {
  }

  // POSITIVE | use an explicit '!= 0' comparison instead
  if (auto value = scalar(); value)
  {
  }

  // POSITIVE | use an explicit '!= nullptr' comparison instead
  if (auto memberPtr = &WithMember::member; memberPtr)
  {
  }

  // POSITIVE | use an explicit '!= 0' comparison instead
  if (auto fraction = fractional(); fraction)
  {
  }

  // The variable is spelled inside a macro, but the conversion is still diagnosed.
  // POSITIVE | use an explicit '!= 0' comparison instead
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
  if (auto fraction = fractional(); fraction != 0)
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
