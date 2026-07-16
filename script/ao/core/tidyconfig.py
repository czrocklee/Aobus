"""clang-tidy base configuration shared by ao tidy.

Pure data module: the CheckOptions values mirror CONTRIBUTING.md conventions. Kept separate
from the tidy command so its unavoidably long option lines need only a local E501 ignore
(see pyproject.toml) instead of relaxing the whole command implementation.
"""

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
  {key: 'readability-identifier-naming.ConstexprVariableIgnoredRegexp', value: '^(rule|value|whitespace|op|name|atom)$'},
  {key: 'readability-identifier-naming.ClassConstantIgnoredRegexp', value: '^(rule|value|whitespace|op|name|atom)$'},
  {key: 'readability-identifier-naming.StaticConstantIgnoredRegexp', value: '^(rule|value|whitespace|op|name|atom)$'},
  {key: 'readability-identifier-naming.FunctionCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodCase', value: 'camelBack'},
  {key: 'readability-identifier-naming.MethodIgnoredRegexp', value: '^property_.*|^signal_.*|^vfunc_.*|^on_.*'},
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
  {key: 'cppcoreguidelines-macro-usage.AllowedRegexp', value: '^DEBUG_*|^[A-Z_]+_LOG_[A-Z_]+$'},
  {key: 'misc-use-internal-linkage.AnalyzeTypes', value: false},
  {key: 'misc-include-cleaner.IgnoreHeaders', value: '.*yaml-cpp.*;.*ryml.*;.*c4[/\\\\].*;.*boost[/\\\\]asio[/\\\\].*;.*boost[/\\\\]interprocess[/\\\\].*;.*boost[/\\\\]system[/\\\\].*;.*boost[/\\\\]unordered[/\\\\].*;.*boost[/\\\\]pfr.*;.*[/\\\\]flat_(set|map);.*[/\\\\]errno.h;.*glib.*;.*Windows Kits.*;.*windows\\.h'}
 ]}"""
