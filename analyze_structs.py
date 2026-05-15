import os
import re

def analyze_file(file_path):
    with open(file_path, 'r') as f:
        content = f.read()
    
    # Simple regex to find struct definitions
    structs = re.finditer(r'struct\s+(\w+)(\s+final)?(\s+:\s+public\s+[\w:]+)?\s*\{', content)
    
    for match in structs:
        struct_name = match.group(1)
        start_pos = match.end()
        
        # Find the closing brace (simplistic matching)
        brace_count = 1
        end_pos = start_pos
        while brace_count > 0 and end_pos < len(content):
            if content[end_pos] == '{':
                brace_count += 1
            elif content[end_pos] == '}':
                brace_count -= 1
            end_pos += 1
        
        struct_body = content[start_pos:end_pos-1]
        
        # Skip PIMPL Impl
        if struct_name == 'Impl':
            continue
            
        # Skip simple Layouts (usually identified by 'static_assert' or 'Layout' in name)
        if 'Layout' in struct_name or 'Header' in struct_name:
            if 'static_assert' in struct_body or 'boost::endian' in struct_body:
                continue

        # Check if it has private or protected members (rare for struct but possible)
        has_private = 'private:' in struct_body or 'protected:' in struct_body
        
        # Count methods
        # Look for '(' but exclude some common patterns
        # Simple constructors are okay, but complex ones might indicate class
        lines = struct_body.split('\n')
        method_lines = []
        for line in lines:
            line = line.strip()
            if '(' in line and ')' in line and ';' in line and not line.startswith('//'):
                # Exclude deleted/defaulted
                if '= delete' in line or '= default' in line:
                    continue
                # Exclude simple operator==
                if 'operator==' in line:
                    continue
                method_lines.append(line)
        
        method_count = len(method_lines)
        
        # If it has methods or private members, it might be a class candidate
        if method_count > 0 or has_private:
             print(f"{file_path}: struct {struct_name} (methods: {method_count}, private: {has_private})")
             for m in method_lines:
                 print(f"  - {m}")

for root, dirs, files in os.walk('.'):
    for file in files:
        if file.endswith('.h') or file.endswith('.hpp'):
            analyze_file(os.path.join(root, file))
