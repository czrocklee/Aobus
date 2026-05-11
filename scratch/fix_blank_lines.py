import re
import sys
import os

def fix_blank_lines(filepath):
    with open(filepath, 'r') as f:
        lines = f.readlines()

    new_lines = []
    modified = False
    
    # Precise pattern for a line that is ONLY a closing brace (plus optional whitespace/comments)
    # This avoids matching lambda arguments like '},', '});', or struct ends '};'
    brace_pattern = re.compile(r'^\s*}\s*(//.*)?$')
    
    # Pattern for keywords that should NOT be preceded by a blank line when following a brace
    keyword_pattern = re.compile(r'^\s*(else|catch|while|case|default|public:|private:|protected:|};|#|namespace)')

    for i in range(len(lines) - 1):
        new_lines.append(lines[i])
        
        curr = lines[i].rstrip()
        next_line = lines[i+1].strip()
        
        # If current line is strictly '}'
        if brace_pattern.match(curr):
            # If next line has content, and is NOT a closing brace, 
            # and does NOT start with a keyword that continues the flow,
            # and is NOT a comma/bracket/paren (expression continuation)
            if next_line and \
               not next_line.startswith('}') and \
               not keyword_pattern.match(next_line) and \
               not any(next_line.startswith(c) for c in [',', ')', ']', ';']):
                
                # Check that we are not at the end of a namespace
                if '// namespace' not in curr:
                    new_lines.append('\n')
                    modified = True
                    
    new_lines.append(lines[-1])
    
    if modified:
        with open(filepath, 'w') as f:
            f.writelines(new_lines)
        return True
    return False

if __name__ == "__main__":
    for arg in sys.argv[1:]:
        if os.path.isfile(arg):
            try:
                if fix_blank_lines(arg):
                    print(f"Fixed: {arg}")
            except Exception as e:
                print(f"Error processing {arg}: {e}")
