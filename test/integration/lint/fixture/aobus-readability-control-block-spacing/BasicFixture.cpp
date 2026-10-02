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

// Directive spellings and continuation lines below are intentional lexer inputs.
// clang-format off
void testControlBlockDirectiveBoundaries(std::int32_t x)
{
  // Conditional-compilation directives are spacing boundaries: no blank line
  // is required before a control statement that follows one and none after a
  // '}' that precedes one. The cases below also prove that genuine
  // adjacent-code errors and non-conditional directives still fire.

#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#ifdef __cplusplus
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#ifndef DIRECTIVE_BOUNDARY_UNDEFINED
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#if defined(__cplusplus) && \
    1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#if 0
  // an inactive branch keeps the active region deterministic
#elif 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#if 0
  // an inactive branch keeps the active region deterministic
#elifdef __cplusplus
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#if 0
  // an inactive branch keeps the active region deterministic
#elifndef DIRECTIVE_BOUNDARY_UNDEFINED
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#elifdef __cplusplus
  // an inactive branch keeps the active region deterministic
#endif

#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#elifndef DIRECTIVE_BOUNDARY_UNDEFINED
  // an inactive branch keeps the active region deterministic
#endif

#if 0
  // an inactive branch keeps the active region deterministic
#else
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#else
  // an inactive branch keeps the active region deterministic
#endif

#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#elif 0
  // an inactive branch keeps the active region deterministic
#endif

#if 1
  // A comment between the directive and the statement stays fine.
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
  // A comment before the closing directive stays fine too.
#endif

#if 1 // A trailing comment on the directive line belongs to the boundary.
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

/* a leading comment */ #if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

# /* a comment between the hash and the name */ ifdef __cplusplus
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
# /* a comment before the closing name */ endif

  x--;
\
#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // NEGATIVE
#endif

  // A null directive must not adopt the following C++ 'if' as its name.
  x++;
#
  if (x > 0) // POSITIVE: FIX-TO: \nif (x > 0)
  {
    x++;
  } // POSITIVE: FIX-TO: }\n
#
  x--;

  // A non-conditional directive is not a spacing boundary and stays checked.
#define DIRECTIVE_BOUNDARY_DEFINE 1
  if (x > 0) // POSITIVE: FIX-TO: \nif (x > 0)
  {
    x++;
  } // POSITIVE: FIX-TO: }\n
#define DIRECTIVE_BOUNDARY_DEFINE_2 2

#if 1
  x--;
  if (x > 0) // POSITIVE: FIX-TO: \nif (x > 0)
  {
    x++;
  } // NEGATIVE
#endif

#if 1
  if (x > 0) // NEGATIVE
  {
    x++;
  } // POSITIVE: FIX-TO: }\n
  x--;
#endif
}
// clang-format on
