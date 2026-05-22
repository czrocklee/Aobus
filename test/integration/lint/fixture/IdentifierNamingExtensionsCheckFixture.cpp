#include "TestHelpers.h"

class NamingDemo
{
public:
  // POSITIVE
  int memberValue = 10;

  // POSITIVE
  int _member_invalid = 5;

  // POSITIVE
  int _InvalidTitle = 6;

  // NEGATIVE
  int _conformingValue = 20;
};

struct StructNamingDemo
{
  // POSITIVE
  int _invalidStructVal = 5;

  // NEGATIVE
  int validStructVal = 10;
};

struct ClassLikeStruct
{
private:
  int _privateVal = 0; // NEGATIVE: Exempt due to private member (considered class-like)
};
