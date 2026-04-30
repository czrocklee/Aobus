# rs_expr
add_gperf_header(
  EXPR_PROPERTY_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/PropertyDispatch.gperf"
  "expr/PropertyDispatch.h"
)
add_gperf_header(
  EXPR_METADATA_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/MetadataDispatch.gperf"
  "expr/MetadataDispatch.h"
)
add_gperf_header(
  EXPR_UNIT_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/UnitDispatch.gperf"
  "expr/UnitDispatch.h"
)

add_library(rs_expr STATIC
    ExecutionPlan.cpp
    PlanEvaluator.cpp
    Expression.cpp
    Parser.cpp
    Serializer.cpp
    "${EXPR_PROPERTY_DISPATCH_HEADER}"
    "${EXPR_METADATA_DISPATCH_HEADER}"
    "${EXPR_UNIT_DISPATCH_HEADER}"
)
target_include_directories(rs_expr PUBLIC ${RS_PUBLIC_INCLUDE_DIR} ${RS_GENERATED_INCLUDE_DIR})
target_link_libraries(rs_expr PUBLIC rs_library)
rockstudio_apply_clang_tidy(rs_expr STRICT)
