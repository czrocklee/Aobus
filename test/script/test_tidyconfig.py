"""Tests for the shared clang-tidy check configuration and argument policy."""

import argparse
import unittest

from ao.core import tidyconfig

_FIX_OPTION_REJECTION = "clang-tidy fix and export options are unsupported; portal analysis is diagnostic-only"
_RESPONSE_FILE_REJECTION = "raw clang-tidy response files are unsupported; pass diagnostic arguments explicitly"


def _assert_rejected(test: unittest.TestCase, argument: str, message: str) -> None:
    """Helper stays ValueError. The adapter copies that text and does not echo the token."""
    with test.assertRaises(ValueError) as helper_error:
        tidyconfig.validate_diagnostic_arguments([argument])
    with test.assertRaises(argparse.ArgumentTypeError) as adapter_error:
        tidyconfig.diagnostic_argument(argument)
    test.assertEqual(str(helper_error.exception), message)
    test.assertEqual(str(adapter_error.exception), message)
    test.assertIsInstance(adapter_error.exception.__cause__, ValueError)
    test.assertEqual(str(adapter_error.exception.__cause__), message)
    test.assertNotIn(argument, str(helper_error.exception))
    test.assertNotIn(argument, str(adapter_error.exception))
    test.assertNotIn("secret", str(helper_error.exception))
    test.assertNotIn("secret", str(adapter_error.exception))


class DiagnosticArgumentValidationTest(unittest.TestCase):
    """The portal commands accept diagnostic-only raw clang-tidy arguments."""

    def test_rejects_every_fix_and_export_option(self):
        for argument in (
            "-fix",
            "--fix",
            "-fix-errors",
            "--fix-errors",
            "-fix-notes",
            "--fix-notes",
            "-export-fixes",
            "--export-fixes",
            "---fix",
            "----export-fixes",
        ):
            with self.subTest(argument=argument):
                _assert_rejected(self, argument, _FIX_OPTION_REJECTION)

    def test_rejects_fix_and_export_options_with_values(self):
        for argument in (
            "-fix=true",
            "--fix=true",
            "-fix-errors=true",
            "--fix-errors=true",
            "-fix-notes=true",
            "--fix-notes=true",
            "-export-fixes=fixes.yaml",
            "--export-fixes=fixes.yaml",
            "---fix=true",
            '"--export-fixes=fixes.yaml"',
        ):
            with self.subTest(argument=argument):
                with self.assertRaises(ValueError) as helper_error:
                    tidyconfig.validate_diagnostic_arguments(["--extra-arg=-std=c++26", argument])
                self.assertEqual(str(helper_error.exception), _FIX_OPTION_REJECTION)
                self.assertNotIn(argument, str(helper_error.exception))
                self.assertNotIn("secret", str(helper_error.exception))
                _assert_rejected(self, argument, _FIX_OPTION_REJECTION)

    def test_rejects_raw_response_files_without_reading_them(self):
        for argument in (
            "@responses.rsp",
            "@nested/path/responses.rsp",
            '"@quoted responses.rsp"',
            "'@single-quoted responses.rsp'",
            "@",
        ):
            with self.subTest(argument=argument):
                _assert_rejected(self, argument, _RESPONSE_FILE_REJECTION)

    def test_rejection_messages_stay_generic_without_leaking_paths(self):
        for argument, message in (
            ("-export-fixes=/tmp/secret-fixes.yaml", _FIX_OPTION_REJECTION),
            ("--export-fixes=/tmp/secret-fixes.yaml", _FIX_OPTION_REJECTION),
            ("@/tmp/secret-responses.rsp", _RESPONSE_FILE_REJECTION),
        ):
            with self.subTest(argument=argument):
                _assert_rejected(self, argument, message)

    def test_diagnostic_argument_returns_the_original_token(self):
        argument = "'--extra-arg=-std=c++26'"
        self.assertIs(tidyconfig.diagnostic_argument(argument), argument)

    def test_bare_fix_word_is_not_a_diagnostic_option(self):
        # Leading dashes are stripped only for option-shaped tokens. A positional
        # or raw word such as "fix" is not a clang-tidy flag and stays allowed.
        for argument in ("fix", "fix-errors", "fix-notes", "export-fixes=fixes.yaml", "-FIX", "--Fix"):
            with self.subTest(argument=argument):
                self.assertIs(tidyconfig.diagnostic_argument(argument), argument)
                tidyconfig.validate_diagnostic_arguments([argument, "--extra-arg=@compiler.rsp"])

    def test_accepts_diagnostic_only_arguments_including_compiler_response_files(self):
        # A compiler response file behind --extra-arg is a compiler-input
        # contract, not a raw clang-tidy response file, and never mutates
        # sources; it stays allowed alongside the ordinary passthrough forms.
        tidyconfig.validate_diagnostic_arguments(
            [
                "--extra-arg=-std=c++26",
                "--extra-arg=-Wno-error",
                "--extra-arg=@compiler.rsp",
                '--extra-arg="@compiler.rsp"',
                '-line-filter=[{"name":"lib/a.cpp","lines":[[1,10]]}]',
                "--vfsoverlay=overlay.json",
                "--header-filter=.*",
            ]
        )


if __name__ == "__main__":
    unittest.main()
