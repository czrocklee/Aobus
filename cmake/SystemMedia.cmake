# The switch exists only where the platform adapter is switchable. Other
# platforms keep no cache entry, so a later platform switch starts from its own
# default instead of inheriting an OFF value that never had an effect there.
if(LINUX)
  option(AOBUS_BUILD_SYSTEM_MEDIA "Build native system-media integration" ON)
else()
  if(AOBUS_BUILD_SYSTEM_MEDIA)
    message(FATAL_ERROR "AOBUS_BUILD_SYSTEM_MEDIA is currently supported only on Linux")
  endif()
  if(DEFINED CACHE{AOBUS_BUILD_SYSTEM_MEDIA})
    # Report the stored value: a stale entry can carry any false spelling
    # (FALSE, NO, 0, <NAME>-NOTFOUND), not just a literal OFF.
    message(STATUS
      "Removing cached AOBUS_BUILD_SYSTEM_MEDIA=$CACHE{AOBUS_BUILD_SYSTEM_MEDIA}: "
      "the switch has no effect on this platform yet")
    unset(AOBUS_BUILD_SYSTEM_MEDIA CACHE)
  endif()
  set(AOBUS_BUILD_SYSTEM_MEDIA OFF)
endif()
