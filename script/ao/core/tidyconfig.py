"""clang-tidy base configuration and check-group policy shared by ao tidy.

Pure data module: the CheckOptions values mirror CONTRIBUTING.md conventions, and the
check groups/disables below are the single policy for both the tidy portal and the lint
fixture runner. Kept separate from the tidy command so its unavoidably long option lines
need only a local E501 ignore (see pyproject.toml) instead of relaxing the whole command
implementation.
"""

# The identifier-naming method exemption exempts exactly two WinRT metadata ABI
# accessors (GetXamlType, GetXmlnsDefinitions) beyond the existing external-method
# list: every projected winrt::_Class_base type repeats them, so they are generated
# ABI surface rather than project naming style. Per-class IDL properties keep
# their local NOLINT markers; property names such as Text and Width stay globally
# non-exempt.
CONFIG_BASE = """
{Checks: 'PLACEHOLDER',
 CheckOptions: [
  {key: 'readability-identifier-length.MinimumVariableNameLength', value: 2},
  {key: 'readability-identifier-length.MinimumParameterNameLength', value: 2},
  {key: 'readability-identifier-length.IgnoredVariableNames', value: '^[_]([^_].*)?$'},
  {key: 'readability-identifier-length.IgnoredBindingNames', value: '^[_]$'},
  {key: 'readability-identifier-naming.ClassCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.StructCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.EnumCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.ScopedEnumConstantCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.ConstexprVariableCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.ConstexprVariablePrefix', value: 'k'},
  {key: 'readability-identifier-naming.ClassConstantCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.ClassConstantPrefix', value: 'k'},
  {key: 'readability-identifier-naming.GlobalConstantCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.GlobalConstantPrefix', value: 'k'},
  {key: 'readability-identifier-naming.StaticConstantCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.StaticConstantPrefix', value: 'k'},
  {key: 'readability-identifier-naming.ConceptCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.FunctionCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodIgnoredRegexp', value: '^(property_.*|signal_.*|vfunc_.*|on_.*|GetAt|Size|IndexOf|GetMany|First|Current|HasCurrent|MoveNext|GetXamlType|GetXmlnsDefinitions)$'},
  {key: 'readability-identifier-naming.PublicMemberCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.ProtectedMemberCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.ProtectedMemberPrefix', value: '_'},
  {key: 'readability-identifier-naming.PrivateMemberCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.PrivateMemberPrefix', value: '_'},
  {key: 'readability-identifier-naming.ParameterCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.LocalVariableCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.TypeAliasCase', value: 'CamelCase'},
  {key: 'readability-identifier-naming.TypeAliasIgnoredRegexp', value: '^(difference_type|value_type|pointer|reference|iterator_category|operand|operation|is_transparent)$'},
  {key: 'readability-magic-numbers.IgnorePowersOf2IntegerValues', value: false},
  {key: 'readability-magic-numbers.IgnoredIntegerValues', value: '0;1;2;3;4;8;8U;16;16U;24;32;32U;64;64U;60;100;1000;1000U'},
  {key: 'readability-qualified-auto.AllowedTypes', value: 'std::array<.*>::(const_)?iterator;std::string_view::(const_)?iterator;.*::iterator;.*Iterator'},
  {key: 'readability-function-cognitive-complexity.Threshold', value: 30},
  {key: 'readability-function-cognitive-complexity.IgnoreMacros', value: true},
  {key: 'readability-simplify-boolean-expr.IgnoreMacros', value: true},
  {key: 'cppcoreguidelines-macro-usage.AllowedRegexp', value: '^DEBUG_*|^[A-Z_]+_LOG_[A-Z_]+$|^AO_(EXPECTS|ENSURES|INVARIANT|FATAL|RT_INVARIANT)$'},
  {key: 'misc-use-internal-linkage.AnalyzeTypes', value: false},
  {key: 'bugprone-suspicious-include.IgnoredRegex', value: '.*\\.g\\.cpp$'},
  {key: 'misc-include-cleaner.IgnoreHeaders', value: '.*yaml-cpp.*;.*ryml.*;.*c4[/\\\\].*;.*boost[/\\\\]asio[/\\\\].*;.*boost[/\\\\]interprocess[/\\\\].*;.*boost[/\\\\]process[/\\\\]v2[/\\\\]process\\.hpp$;.*boost[/\\\\]system[/\\\\].*;.*boost[/\\\\]unordered[/\\\\].*;.*boost[/\\\\]pfr.*;.*[/\\\\]flat_(set|map);.*[/\\\\]errno.h;.*glib.*;.*Windows Kits.*;.*windows\\.h;.*[/\\\\]pch\\.h;.*[/\\\\]winrt[/\\\\].*;.*ao[/\\\\]async[/\\\\]detail[/\\\\]BoostAsioTsanPrelude\\.h$;.*ao[/\\\\]utility[/\\\\]StrongTypeFormatter\\.h$;.*sys[/\\\\]_types[/\\\\]_pid_t\\.h$;.*sys[/\\\\]_types[/\\\\]_sigset_t\\.h$;.*sys[/\\\\]fcntl\\.h$;.*sys[/\\\\]signal\\.h$;.*\\.g\\.h'}
 ]}"""

# Check groups: start from nothing, enable curated groups, then disable known false
# positives or project-preference conflicts.
STRICT_CHECKS = ",".join(
    [
        "-*",
        # === enabled check groups ===
        "aobus-*",  # Aobus custom checks (naming, spacing, etc.)
        "bugprone-*",  # bug-prone pattern detection
        "performance-*",  # performance issues
        "cppcoreguidelines-*",  # C++ Core Guidelines
        "misc-*",  # miscellaneous useful checks
        "modernize-*",  # modern C++ usage
        "readability-*",  # readability improvements
        "portability-*",  # portability concerns
        "objc-avoid-nserror-init",
        "objc-dealloc-in-category",
        "objc-forbidden-subclassing",
        "objc-missing-hash",
        "objc-nsinvocation-argument-lifetime",
        "objc-property-declaration",
        "objc-super-self",
        "google-objc-avoid-throwing-exception",
        "google-build-namespaces",
        "google-readability-casting",
        "google-readability-namespace-comments",
        # === disabled: false positives or project preference ===
        "-bugprone-easily-swappable-parameters",  # frequent false positives
        # CRTP customization deliberately refines non-virtual fallback methods
        # (RecursiveASTVisitor and std::ranges::view_interface).
        "-bugprone-derived-method-shadowing-base-method",
        "-bugprone-exception-escape",  # dominated by allocation paths and MSVC container moves
        "-bugprone-throwing-static-initialization",  # dominated by STL startup-allocation details
        "-cppcoreguidelines-avoid-magic-numbers",  # handled by aobus readability check
        "-cppcoreguidelines-avoid-const-or-ref-data-members",  # common pattern for views
        "-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access",  # .at() is not a universal hot-path policy
        "-cppcoreguidelines-pro-bounds-constant-array-index",  # table dispatch is fine
        "-cppcoreguidelines-pro-bounds-pointer-arithmetic",  # common in layout/audio code
        "-misc-no-recursion",  # recursion is idiomatic in some modules
        "-misc-multiple-inheritance",  # renamed Fuchsia policy conflicts with GTK/framework composition
        "-misc-non-private-member-variables-in-classes",  # impl structs use this pattern
        "-modernize-use-trailing-return-type",  # style choice: prefer leading type
        "-modernize-use-nodiscard",  # not enforced globally
        "-modernize-use-auto",  # handled by aobus custom check
        "-performance-unnecessary-value-param",  # too noisy for interface types
        "-performance-move-const-arg",  # conflicts with const-correctness
        "-readability-redundant-member-init",  # explicit init is intentional
        "-readability-convert-member-functions-to-static",  # prefers explicit design intent
        "-portability-avoid-pragma-once",  # project standard is #pragma once
        "-clang-diagnostic-*",  # compiler diagnostics not our domain
    ]
)

RELAXED_CHECKS = ",".join(
    [
        STRICT_CHECKS,
        # === additionally disabled for test code ===
        "-bugprone-unchecked-optional-access",  # test code uses .value() liberally
        "-bugprone-unused-return-value",  # tests often discard return values
        "-cppcoreguidelines-avoid-c-arrays",  # Catch2 API and test data arrays
        "-cppcoreguidelines-avoid-magic-numbers",  # not enforced in tests
        "-cppcoreguidelines-pro-type-reinterpret-cast",  # common pattern in test mocks
        "-cppcoreguidelines-pro-type-vararg",  # debug helpers use printf-style
        "-portability-template-virtual-member-function",  # test fixtures use vfunc_* pattern
        "-modernize-use-designated-initializers",  # positional expected-data tables are clearer in tests
        "-readability-function-cognitive-complexity",  # test bodies are inherently complex
        "-readability-identifier-length",  # test locals (id, v, it) are fine
        "-readability-magic-numbers",  # not enforced in tests
    ]
)


def mode_disabled_checks(mode: str) -> list[str]:
    """The explicit disable directives a mode applies, e.g. ['-bugprone-...', ...]."""
    source = RELAXED_CHECKS if mode == "RELAXED" else STRICT_CHECKS
    return [token for token in source.split(",") if token.startswith("-") and token != "-*"]


def checks_for(mode: str, selected: str | None) -> str:
    """Resolve the clang-tidy check list for a mode.

    With --check, run only the selected check(s) but still honour the mode's disable
    list, so a check disabled for tests (e.g. readability-magic-numbers) stays off there.
    """
    if selected:
        return ",".join(["-*", selected, *mode_disabled_checks(mode)])
    return RELAXED_CHECKS if mode == "RELAXED" else STRICT_CHECKS


def config_for(checks: str) -> str:
    """Serialize the base config with a check list, compacted identically for every caller."""
    return " ".join(CONFIG_BASE.replace("PLACEHOLDER", checks).split())
