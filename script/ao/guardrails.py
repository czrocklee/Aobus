"""Standalone shared completion-audit CLI; does not configure a build system."""

import argparse
import json
import re
import sys
from dataclasses import asdict
from pathlib import Path

# CMake executes this file directly; Bazel and the portal can import the API.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).absolute().parent.parent))

from ao.core import guardrails


def _regex(args: argparse.Namespace, name: str) -> str | None:
    path = getattr(args, name + "_file")
    return Path(path).read_text(encoding="utf-8") if path else (getattr(args, name) or None)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--platform", default=sys.platform)
    parser.add_argument("--audit", action="append")
    parser.add_argument("--no-gtk", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument(
        "--kind",
        choices=(
            "forbidden",
            "architecture-rules",
            "architecture-self-test",
            "uimodel-organization",
            "uimodel-neutrality",
            "gtk-leaf",
            "winui-leaf",
            "winui-events",
            "winui-state",
            "source-files",
        ),
    )
    parser.add_argument("--root", action="append", type=Path)
    parser.add_argument("--roots-list", help=argparse.SUPPRESS)
    parser.add_argument("--public-root", type=Path)
    parser.add_argument("--uimodel-source-root", type=Path)
    parser.add_argument("--test-root", type=Path)
    parser.add_argument("--mode", default="enforce")
    for name in ("forbidden", "allowed", "exclude"):
        parser.add_argument("--" + name)
        parser.add_argument("--" + name + "-file")
    args = parser.parse_args(argv)
    try:
        if args.kind == "source-files":
            if not args.root:
                raise guardrails.AuditInputError("ROOT not specified")
            print(";".join(path.as_posix() for path in guardrails.source_files(args.root[0])), end="")
            return 0
        if args.kind == "forbidden":
            options = {
                name: value for name in ("forbidden", "allowed", "exclude") if (value := _regex(args, name)) is not None
            }
            roots = [Path(root) for root in args.roots_list.split(";")] if args.roots_list else (args.root or ())
            result = guardrails.scan_forbidden(roots, mode=args.mode, **options)
        elif args.kind in ("uimodel-organization", "uimodel-neutrality"):
            result = guardrails.audit_uimodel(args.kind, args.public_root, args.uimodel_source_root, args.test_root)
        elif args.kind in ("gtk-leaf", "winui-leaf", "winui-events", "winui-state"):
            result = guardrails.audit_frontend(args.kind, args.root[0] if args.root else None)
        else:
            if args.source_root is None:
                raise guardrails.AuditInputError("AOBUS_SOURCE_DIR must name the repository root")
            gtk = not args.no_gtk and args.platform == "linux"
            if args.list:
                print(
                    json.dumps([asdict(audit) for audit in guardrails.inventory(args.source_root, gtk=gtk)], indent=2)
                )
                return 0
            if args.kind == "architecture-self-test":
                guardrails.architecture_self_test()
                print("Application architecture rule self-test passed")
                return 0
            if args.kind == "architecture-rules":
                result = guardrails.audit_architecture_rules(args.source_root)
            elif args.output_dir:
                if args.audit:
                    raise guardrails.AuditInputError("--output-dir runs the complete inventory; do not use --audit")
                completion = guardrails.run(
                    args.source_root, args.output_dir, args.platform, gtk=False if args.no_gtk else None
                )
                print(f"Guardrail report: {completion.report_path}")
                result = guardrails.AuditResult(completion.errors, completion.warnings)
            else:
                result = guardrails.run_audits(args.source_root, args.audit, gtk=gtk)
        for finding in result.errors:
            print(finding, file=sys.stderr)
        for finding in result.warnings:
            print(f"WARNING: {finding}", file=sys.stderr)
        if args.kind == "architecture-rules" and result.errors:
            print(f"Application architecture audit found {len(result.errors)} violation(s)", file=sys.stderr)
        if args.kind == "forbidden" and result.warnings:
            print(f"WARNING: report mode found {len(result.warnings)} forbidden dependency match(es)", file=sys.stderr)
        return 0 if result.ok else 1
    except (guardrails.AuditInputError, OSError, re.error) as exc:
        print(f"Guardrail input error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
