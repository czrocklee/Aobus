#include <lexy/dsl.hpp>
#include <string>

namespace dsl = lexy::dsl;

constexpr auto invalid_chars = dsl::ascii::space | dsl::lit_c<'"'> | dsl::lit_c<'\''>
    | dsl::lit_c<'$'> | dsl::lit_c<'@'> | dsl::lit_c<'#'> | dsl::lit_c<'%'>
    | dsl::lit_c<'!'> | dsl::lit_c<'('> | dsl::lit_c<')'> | dsl::lit_c<'&'> | dsl::lit_c<'|'>
    | dsl::lit_c<'='> | dsl::lit_c<'~'> | dsl::lit_c<'<'> | dsl::lit_c<'>'>
    | dsl::lit_c<'+'> | dsl::lit_c<'-'>;

constexpr auto rule = dsl::capture(dsl::plus(dsl::code_point - invalid_chars));
