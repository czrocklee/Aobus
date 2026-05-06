import sys
import re

def filter_message():
    try:
        msg = sys.stdin.read()
        if not msg:
            return
            
        lines = msg.split('\n')
        filtered_lines = []
        
        for line in lines:
            # Case-insensitive check for Co-Authored-By:
            if re.search(r'Co-Authored-By:', line, re.IGNORECASE):
                continue
            # Check for noreply@anthropic.com
            if 'noreply@anthropic.com' in line:
                continue
            filtered_lines.append(line)
            
        # Remove trailing empty lines
        while filtered_lines and not filtered_lines[-1].strip():
            filtered_lines.pop()
            
        # Write the filtered message back to stdout
        output = '\n'.join(filtered_lines)
        sys.stdout.write(output)
        # Git usually expects a newline at the end if the file ended with one.
        # But we want to be clean.
        if output and not output.endswith('\n'):
            sys.stdout.write('\n')
            
    except Exception as e:
        sys.stderr.write(f"Error filtering message: {e}\n")
        sys.exit(1)

if __name__ == '__main__':
    filter_message()
