# rs_lmdb
add_library(rs_lmdb STATIC
    Environment.cpp
    Transaction.cpp
    Database.cpp
)
target_include_directories(rs_lmdb PUBLIC ${RS_PUBLIC_INCLUDE_DIR})
target_link_libraries(rs_lmdb PUBLIC lmdb)
rockstudio_apply_clang_tidy(rs_lmdb STRICT)
