#!/usr/bin/env python3
import sys
import re
import argparse
from collections import defaultdict

def parse_clang_tidy_output(output, filter_check=None):
    # Match lines like: path/to/file.cpp:123:4: warning: message [check-name]
    pattern = re.compile(r'^([^:]+):(\d+):(\d+): warning: (.*) \[(.*)\]$')
    diagnostics = defaultdict(list)
    for line in output.splitlines():
        match = pattern.match(line)
        if match:
            file_path, line_num, col, message, check_name = match.groups()
            # We record ALL diagnostics found in the output.
            # The validation logic will then check if they were expected.
            diagnostics[int(line_num)].append(check_name)
    return diagnostics

def parse_source_file(file_path, filter_check=None):
    # Match comments like: // POSITIVE: aobus-check-name
    # Or just // POSITIVE.
    # If POSITIVE is the only thing on a line, it applies to the NEXT line.
    # If POSITIVE is on a line with code, it applies to THAT line.
    pattern = re.compile(r'//\s*POSITIVE(?::\s*([a-zA-Z0-9\-_]+))?')
    expected = defaultdict(list)
    with open(file_path, 'r') as f:
        for i, line in enumerate(f, 1):
            match = pattern.search(line)
            if match:
                check_name = match.group(1) or filter_check
                if not check_name:
                    continue
                
                # Determine if the comment is "standalone" (only whitespace/comment on line)
                # or "inline" (attached to code).
                code_part = line.split('//')[0].strip()
                target_line = i if code_part else i + 1
                expected[target_line].append(check_name)
                
    return expected

def main():
    parser = argparse.ArgumentParser(description='Verify clang-tidy diagnostics against inline comments.')
    parser.add_argument('source', help='Source file with POSITIVE comments')
    parser.add_argument('--check', help='Specific check name being tested (all diagnostics in output must match this or be expected)')
    parser.add_argument('--input', help='File containing clang-tidy output (defaults to stdin)')
    args = parser.parse_args()

    if args.input:
        with open(args.input, 'r') as f:
            tidy_output = f.read()
    else:
        tidy_output = sys.stdin.read()

    actual_diagnostics = parse_clang_tidy_output(tidy_output)
    expected_diagnostics = parse_source_file(args.source, filter_check=args.check)

    all_lines = sorted(set(actual_diagnostics.keys()) | set(expected_diagnostics.keys()))
    errors = []

    for line in all_lines:
        actual = actual_diagnostics.get(line, [])
        expected = expected_diagnostics.get(line, [])

        # Check for unexpected diagnostics
        for a in actual:
            if a not in expected:
                # If we are testing a specific check, ANY other diagnostic is an error,
                # even if it's not on a POSITIVE line.
                errors.append(f"Unexpected diagnostic at {args.source}:{line}: [{a}]")
        
        # Check for missing diagnostics
        for e in expected:
            # If we are filtering by check, we only care about missing ones of that type.
            if args.check and e != args.check:
                continue
            if e not in actual:
                errors.append(f"Missing expected diagnostic at {args.source}:{line}: [{e}]")

    if errors:
        for err in errors:
            print(err, file=sys.stderr)
        sys.exit(1)
    else:
        print(f"SUCCESS: All diagnostics verified for {args.source}")

if __name__ == '__main__':
    main()
