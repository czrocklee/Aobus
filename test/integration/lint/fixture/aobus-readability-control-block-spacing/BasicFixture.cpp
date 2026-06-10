// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

void testControlBlockSpacing(std::int32_t x)
{
  // NEGATIVE
  if (x > 0)
  {
    x++;
  }

  // NEGATIVE
  for (std::int32_t i = 0; i < 10; ++i)
  {
    x += i;
  }

  // NEGATIVE
  while (x > 100)
  {
    x--;
  }

  // NEGATIVE
  switch (x)
  {
    default: break;
  }

  if (x > 0)
  {
    x++;
  }

  if (x > 0)
  {
    x++;
  }
  else if (x < -10)
  {
    x--;
  }

  // NEGATIVE - standalone while after if with proper spacing (not a do-while)
  if (x > 0)
  {
    x++;
  }

  while (x > 100)
  {
    x--;
  }

  if (x > 0)
  {
    x++;
  } // POSITIVE: FIX-TO: }\n
  x--;

  x++;
  if (x > 0) // POSITIVE: FIX-TO: \nif (x > 0)
  {
    x++;
  }

  x++;
  for (std::int32_t i = 0; i < 10; ++i) // POSITIVE: FIX-TO: \nfor (std::int32_t i = 0; i < 10; ++i)
  {
    x += i;
  }

  x++;
  while (x > 100) // POSITIVE: FIX-TO: \nwhile (x > 100)
  {
    x--;
  }

  x++;
  switch (x) // POSITIVE: FIX-TO: \nswitch (x)
  {
    default: break;
  }

  x++;
  do // POSITIVE: FIX-TO: \ndo
  {
    x--;
  }
  while (x > 0);

  for (std::int32_t i = 0; i < 10; ++i)
  {
    x += i;
  } // POSITIVE: FIX-TO: }\n
  x++;

  while (x > 100)
  {
    x--;
  } // POSITIVE: FIX-TO: }\n
  x++;

  switch (x)
  {
    default: break;
  } // POSITIVE: FIX-TO: }\n
  x++;

  do
  {
    x--;
  } // POSITIVE: FIX-TO: }\n  while (x > 0);\n
  while (x > 0);
  x++;
}

void testControlBlockCommentSpacing(std::int32_t x)
{
  // This is some description

  if (x > 0) // POSITIVE
  {
    x++;
  }

  // This is some description
  if (x > 0)
  {
    x++;
  }
}

void testControlBlockTryCatchSpacing(std::int32_t x)
{
  x++;
  try // POSITIVE: FIX-TO: \ntry
  {
    if (x > 0)
    {
      throw 42;
    }
  }
  catch (std::int32_t)
  {
    x = 0;
  }

  try
  {
    throw 42;
  }
  catch (std::int32_t)
  {
    x = 1;
  } // POSITIVE: FIX-TO: }\n
  x--;

  try
  {
    throw 42;
  }
  catch (std::int32_t)
  {
    x = 2;
  }

  try
  {
    throw 42;
  }
  catch (std::int32_t)
  {
    x = 3;
  }
  catch (...)
  {
    x = 4;
  }
}
