// SPDX-License-Identifier: MIT

#if !__has_include(<lmdb.h>) || !__has_include(<fakeit.hpp>)
#error Missing the selected LMDB or FakeIt interface
#endif
#if !__has_include(<opus/opus.h>) || !__has_include(<opus_multistream.h>)
#error Missing one of the selected Opus include roots
#endif
#if __has_include(<../include/lmdb.h>) || __has_include(<../lib/pkgconfig/lmdb.pc>)
#error LMDB exposes its provider layout outside the projected include root
#endif
#if __has_include(<../standalone/fakeit.hpp>) || __has_include(<standalone/fakeit.hpp>)
#error FakeIt exposes an unselected integration
#endif

int main()
{
  return 0;
}
