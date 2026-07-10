# ao_query
add_gperf_header(
  QUERY_UNIT_DISPATCH_HEADER
  "${CMAKE_CURRENT_SOURCE_DIR}/UnitDispatch.gperf"
  "query/UnitDispatch.h"
)

add_library(ao_query STATIC
    Completion.cpp
    detail/CompletionTokenizer.cpp
    ExecutionPlan.cpp
    FieldCatalog.cpp
    Field.cpp
    FormatExpression.cpp
    PlanEvaluator.cpp
    Expression.cpp
    Parser.cpp
    Predicate.cpp
    Serializer.cpp
    "${QUERY_UNIT_DISPATCH_HEADER}"
)
target_include_directories(ao_query PUBLIC ${AO_PUBLIC_INCLUDE_DIR} ${AO_GENERATED_INCLUDE_DIR})
target_link_libraries(ao_query PUBLIC ao_library)
target_link_libraries(ao_query PRIVATE foonathan::lexy)
