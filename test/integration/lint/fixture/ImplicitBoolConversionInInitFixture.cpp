// RUN: %check_clang_tidy %s aobus-implicit-bool-conversion-in-init %t

int scalar();
void* raw();

void test()
{
  // POSITIVE
  if (auto ptr = raw(); ptr)
  {
  }

  // POSITIVE
  if (auto value = scalar(); value)
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
