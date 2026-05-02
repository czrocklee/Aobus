# ao_query
add_gperf_header(
  QUERY_PROPERTY_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/PropertyDispatch.gperf"
  "query/PropertyDispatch.h"
)
add_gperf_header(
  QUERY_METADATA_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/MetadataDispatch.gperf"
  "query/MetadataDispatch.h"
)
add_gperf_header(
  QUERY_UNIT_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/UnitDispatch.gperf"
  "query/UnitDispatch.h"
)

add_library(ao_query STATIC
    ExecutionPlan.cpp
    PlanEvaluator.cpp
    Expression.cpp
    Parser.cpp
    Serializer.cpp
    "${QUERY_PROPERTY_DISPATCH_HEADER}"
    "${QUERY_METADATA_DISPATCH_HEADER}"
    "${QUERY_UNIT_DISPATCH_HEADER}"
)
target_include_directories(ao_query PUBLIC ${AO_PUBLIC_INCLUDE_DIR} ${AO_GENERATED_INCLUDE_DIR})
target_link_libraries(ao_query PUBLIC ao_library)
aobus_apply_clang_tidy(ao_query STRICT)
