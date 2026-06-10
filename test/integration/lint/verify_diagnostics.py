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
    # Match comments like: // POSITIVE: aobus-check-name or // NEGATIVE: aobus-check-name
    # Or just // POSITIVE / // NEGATIVE.
    # If the annotation is the only thing on a line, it applies to the NEXT line.
    # If the annotation is on a line with code, it applies to THAT line.
    pos_pattern = re.compile(r'//\s*POSITIVE(?::\s*([a-zA-Z0-9\-_]+))?')
    neg_pattern = re.compile(r'//\s*NEGATIVE(?::\s*([a-zA-Z0-9\-_]+))?')
    
    expected = defaultdict(list)
    negated = defaultdict(list)
    with open(file_path, 'r') as f:
        for i, line in enumerate(f, 1):
            pos_match = pos_pattern.search(line)
            neg_match = neg_pattern.search(line)
            
            if pos_match or neg_match:
                code_part = line.split('//')[0].strip()
                target_line = i if code_part else i + 1
                
                if pos_match:
                    check_name = pos_match.group(1) or filter_check
                    if check_name == 'FIX-TO':
                        check_name = filter_check
                    if check_name:
                        expected[target_line].append(check_name)
                        
                if neg_match:
                    check_name = neg_match.group(1) or filter_check
                    if check_name == 'FIX-TO':
                        check_name = filter_check
                    if check_name:
                        negated[target_line].append(check_name)
                
    return expected, negated

def check_fixed_content(comment_line, expected_str, fixed_lines, idx):
    # Replace literal \n with actual newline
    expected_str = expected_str.replace('\\n', '\n')
    
    # Split expected_str into lines
    expected_lines = expected_str.split('\n')
    
    is_inline = False
    comment_part = comment_line.split('//')[0].strip()
    if comment_part:
        is_inline = True
        
    if expected_str.startswith('\n'):
        # Check that the line before the comment is empty
        if idx - 1 < 0 or fixed_lines[idx - 1].strip() != '':
            return f"Expected empty line before comment at line {idx+1}"
        expected_lines = expected_lines[1:]
        
    start_match_idx = idx if is_inline else idx + 1
    
    for offset, expected_line in enumerate(expected_lines):
        match_idx = start_match_idx + offset
        if match_idx >= len(fixed_lines):
            return f"Expected line '{expected_line}' but reached end of file"
            
        actual_line = fixed_lines[match_idx]
        if is_inline and offset == 0:
            actual_line = actual_line.split('//')[0]
            
        if actual_line.strip() != expected_line.strip():
            return f"Expected '{expected_line.strip()}' at line {match_idx+1}, but got '{actual_line.strip()}'"
            
    return None

def verify_expected_fixes(source_path, fixed_path):
    # Match comment line containing: // POSITIVE: FIX-TO: expected_text or // FIX-TO: expected_text
    fix_to_pattern = re.compile(r'//\s*(?:POSITIVE:\s*)?FIX-TO:\s*(.*)')
    
    expected_fixes = []
    with open(source_path, 'r') as f:
        for line in f:
            match = fix_to_pattern.search(line)
            if match:
                expected_fixes.append((line.strip(), match.group(1)))
                
    if not expected_fixes:
        return []
        
    with open(fixed_path, 'r') as f:
        fixed_lines = f.readlines()
        
    errors = []
    current_idx = 0
    for comment_text, expected_str in expected_fixes:
        found = False
        for idx in range(current_idx, len(fixed_lines)):
            if comment_text in fixed_lines[idx]:
                found = True
                current_idx = idx + 1
                
                # Perform verification
                err = check_fixed_content(fixed_lines[idx], expected_str, fixed_lines, idx)
                if err:
                    errors.append(f"Fix verification failed for '{comment_text}' in {source_path}:\n  {err}")
                break
        if not found:
            errors.append(f"Expected comment line not found in fixed file: '{comment_text}'")
            
    return errors

def main():
    parser = argparse.ArgumentParser(description='Verify clang-tidy diagnostics against inline comments.')
    parser.add_argument('source', help='Source file with POSITIVE comments')
    parser.add_argument('--check', help='Specific check name being tested (all diagnostics in output must match this or be expected)')
    parser.add_argument('--input', help='File containing clang-tidy output (defaults to stdin)')
    parser.add_argument('--fixed-file', help='Verify that the auto-fixed file matches FIX-TO comments')
    parser.add_argument('--only-fixes', action='store_true', help='Only verify the auto-fixes, do not check diagnostics')
    args = parser.parse_args()

    if args.only_fixes:
        errors = []
        if args.fixed_file:
            errors.extend(verify_expected_fixes(args.source, args.fixed_file))
        if errors:
            for err in errors:
                print(err, file=sys.stderr)
            sys.exit(1)
        else:
            print(f"SUCCESS: All fixes verified for {args.source}")
            sys.exit(0)

    if args.input:
        with open(args.input, 'r') as f:
            tidy_output = f.read()
    else:
        tidy_output = sys.stdin.read()

    actual_diagnostics = parse_clang_tidy_output(tidy_output)
    expected_diagnostics, negated_diagnostics = parse_source_file(args.source, filter_check=args.check)

    all_lines = sorted(set(actual_diagnostics.keys()) | set(expected_diagnostics.keys()) | set(negated_diagnostics.keys()))
    errors = []

    if args.fixed_file:
        errors.extend(verify_expected_fixes(args.source, args.fixed_file))

    for line in all_lines:
        actual = actual_diagnostics.get(line, [])
        expected = expected_diagnostics.get(line, [])

        # Check for unexpected diagnostics
        for a in actual:
            if a not in expected:
                if a in negated_diagnostics.get(line, []):
                    errors.append(f"Diagnostic found on explicitly NEGATIVE line at {args.source}:{line}: [{a}]")
                else:
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
