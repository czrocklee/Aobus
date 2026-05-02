# ao_lmdb
add_library(ao_lmdb STATIC
    Environment.cpp
    Transaction.cpp
    Database.cpp
)
target_include_directories(ao_lmdb PUBLIC ${AO_PUBLIC_INCLUDE_DIR})
target_link_libraries(ao_lmdb PUBLIC lmdb)
aobus_apply_clang_tidy(ao_lmdb STRICT)
