#include "TestHelpers.h"

void testControlBlockSpacing(int x)
{
  // NEGATIVE
  if (x > 0)
  {
    x++;
  }

  // NEGATIVE
  for (int i = 0; i < 10; ++i)
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

  if (x > 0)
  {
    x++;
  } // POSITIVE
  x--;

  x++;
  if (x > 0) // POSITIVE
  {
    x++;
  }

  x++;
  for (int i = 0; i < 10; ++i) // POSITIVE
  {
    x += i;
  }

  x++;
  while (x > 100) // POSITIVE
  {
    x--;
  }

  x++;
  switch (x) // POSITIVE
  {
    default: break;
  }

  x++;
  do // POSITIVE
  {
    x--;
  }
  while (x > 0);

  for (int i = 0; i < 10; ++i)
  {
    x += i;
  } // POSITIVE
  x++;

  while (x > 100)
  {
    x--;
  } // POSITIVE
  x++;

  switch (x)
  {
    default: break;
  } // POSITIVE
  x++;

  do
  {
    x--;
  } // POSITIVE
  while (x > 0);
  x++;
}

void testControlBlockCommentSpacing(int x)
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

void testControlBlockTryCatchSpacing(int x)
{
  x++;
  try // POSITIVE
  {
    if (x > 0)
    {
      throw 42;
    }
  }
  catch (int)
  {
    x = 0;
  }

  try
  {
    throw 42;
  }
  catch (int)
  {
    x = 1;
  } // POSITIVE
  x--;

  try
  {
    throw 42;
  }
  catch (int)
  {
    x = 2;
  }

  try
  {
    throw 42;
  }
  catch (int)
  {
    x = 3;
  }
  catch (...)
  {
    x = 4;
  }
}
