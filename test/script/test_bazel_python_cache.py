"""Source-backed cache redirection and the retained sourceless exception."""

import json
import os
import py_compile
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

_IMPORT_PROBE = textwrap.dedent("""\
    import importlib
    import json
    import os
    import sys
    from pathlib import Path

    opens = []
    writes = []
    def audit(event, values):
        if event == 'open' and isinstance(values[0], (str, bytes)):
            path = os.path.abspath(os.fsdecode(values[0]))
            opens.append(path)
            if values[2] & (os.O_WRONLY | os.O_RDWR | os.O_CREAT):
                writes.append(path)
    sys.addaudithook(audit)
    sys.path.insert(0, sys.argv[1])
    modules = {name: importlib.import_module(name) for name in
               ('timestamp_collision', 'unchecked_hash', 'without_cache', 'legacy_only')}
    print(json.dumps({
        'values': {name: module.VALUE for name, module in modules.items()},
        'loaders': {name: type(module.__loader__).__name__ for name, module in modules.items()},
        'opens': opens,
        'writes': writes,
        'prefix': sys.pycache_prefix,
        'flags': [sys.flags.isolated, sys.flags.no_site, sys.flags.dont_write_bytecode, sys.flags.utf8_mode],
    }))
""")

_PROJECT_IMPORT_PROBE = textwrap.dedent("""\
    import json
    import os
    import sys
    from pathlib import Path

    sys.path.insert(0, sys.argv[1])
    repository = Path(sys.argv[2])
    files = set()
    directories = set()
    def audit(event, values):
        if values and isinstance(values[0], (str, bytes)):
            if event == 'open':
                files.add(os.path.abspath(os.fsdecode(values[0])))
            elif event in {'os.listdir', 'os.scandir'}:
                directories.add(os.path.abspath(os.fsdecode(values[0])))
    sys.addaudithook(audit)
    from ao.core.bazel_resolver import _external_input
    from ao.core import bazel_workspace, bazel_deps, bazel_headers
    print(json.dumps({
        'files': sorted(
            path for path in files
            if _external_input(path, repository) and not path.startswith('/nix/store/')
        ),
        'directories': sorted(
            path for path in directories
            if _external_input(path, repository) and not path.startswith('/nix/store/')
        ),
    }))
""")


class PythonCachePrefixTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-python cache with spaces-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.modules = self.root / "modules with spaces"
        self.modules.mkdir()
        self.prefix = self.root / "repository/_tmp"
        self.prefix.mkdir(parents=True)
        (self.prefix / ".keep").touch()
        self.caches = {}
        for name, mode in (
            ("timestamp_collision", py_compile.PycInvalidationMode.TIMESTAMP),
            ("unchecked_hash", py_compile.PycInvalidationMode.UNCHECKED_HASH),
        ):
            source = self.modules / (name + ".py")
            source.write_bytes(b"VALUE = 'cached'\n")
            original = source.stat()
            cache = self.modules / "__pycache__" / f"{name}.{sys.implementation.cache_tag}.pyc"
            py_compile.compile(str(source), cfile=str(cache), doraise=True, invalidation_mode=mode)
            # Equal size and mtime deliberately leave the timestamp cache valid.
            source.write_bytes(b"VALUE = 'source'\n")
            os.utime(source, ns=(original.st_atime_ns, original.st_mtime_ns))
            self.caches[name] = cache
        (self.modules / "without_cache.py").write_bytes(b"VALUE = 'fresh source'\n")
        seed = self.root / "legacy_seed.py"
        seed.write_bytes(b"VALUE = 'sourceless'\n")
        self.legacy = self.modules / "legacy_only.pyc"
        py_compile.compile(str(seed), cfile=str(self.legacy), doraise=True)

    def probe(self, *, prefix):
        argv = [sys.executable, "-I", "-S", "-B", "-X", "utf8"]
        if prefix:
            argv.extend(["-X", "pycache_prefix=" + str(self.prefix)])
        argv.extend(["-c", _IMPORT_PROBE, str(self.modules)])
        result = subprocess.run(
            argv, executable=sys.executable, cwd=self.root, capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["flags"], [1, 1, 1, 1])
        return data

    def test_directory_prefix_reads_source_instead_of_valid_timestamp_and_unchecked_caches(self):
        ordinary = self.probe(prefix=False)
        redirected = self.probe(prefix=True)
        self.assertEqual(redirected["prefix"], str(self.prefix))
        for name, cache in self.caches.items():
            with self.subTest(module=name):
                self.assertEqual(ordinary["values"][name], "cached")
                self.assertIn(str(cache), ordinary["opens"])
                self.assertEqual(redirected["values"][name], "source")
                self.assertIn(str(self.modules / (name + ".py")), redirected["opens"])
                self.assertNotIn(str(cache), redirected["opens"])
        prefix_candidates = [
            path for path in redirected["opens"] if Path(path).is_relative_to(self.prefix) and path.endswith(".pyc")
        ]
        self.assertGreaterEqual(len(prefix_candidates), len(self.caches))

    def test_dont_write_bytecode_prevents_automatic_cache_writes_with_directory_prefix(self):
        redirected = self.probe(prefix=True)
        self.assertEqual(redirected["values"]["without_cache"], "fresh source")
        self.assertEqual(redirected["writes"], [])
        self.assertEqual(set(self.prefix.rglob("*")), {self.prefix / ".keep"})
        self.assertEqual(set(self.modules.rglob("*.pyc")), {*self.caches.values(), self.legacy})

    def test_sourceless_bytecode_still_loads_and_is_a_real_read_input(self):
        for prefix in (False, True):
            with self.subTest(prefix=prefix):
                observed = self.probe(prefix=prefix)
                self.assertEqual(observed["values"]["legacy_only"], "sourceless")
                self.assertEqual(observed["loaders"]["legacy_only"], "SourcelessFileLoader")
                self.assertIn(str(self.legacy), observed["opens"])

    def test_project_imports_and_package_searches_are_named_by_the_audited_inventory(self):
        project = Path(__file__).resolve().parents[2]
        environment = dict(os.environ, AOBUS_CHECKOUT_ID="scratch-cache-inventory")
        result = subprocess.run(
            [
                sys.executable,
                "-I",
                "-S",
                "-B",
                "-X",
                "utf8",
                "-X",
                "pycache_prefix=" + str(self.prefix),
                "-c",
                _PROJECT_IMPORT_PROBE,
                str(project / "script"),
                str(self.prefix.parent),
            ],
            executable=sys.executable,
            cwd=self.prefix.parent,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        inventory = json.loads(result.stdout)
        core = project / "script/ao/core"
        files = set(inventory["files"])
        imported_sources = {path for path in files if Path(path).is_relative_to(project) and path.endswith(".py")}
        # Repository rules watch exactly this inventory, not a static module list.
        self.assertLessEqual(
            {
                str(core.parent / "__init__.py"),
                str(core / "__init__.py"),
                str(core / "bazel_workspace.py"),
                str(core / "bazel_deps.py"),
                str(core / "bazel_headers.py"),
            },
            imported_sources,
        )
        self.assertNotIn(str(core / "tidyengine.py"), imported_sources)
        self.assertFalse(any(Path(path).is_relative_to(project) and path.endswith(".pyc") for path in files))
        self.assertLessEqual({str(project / "script"), str(core.parent), str(core)}, set(inventory["directories"]))


if __name__ == "__main__":
    unittest.main()
