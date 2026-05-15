import os
import re

def analyze_file(file_path):
    with open(file_path, 'r') as f:
        content = f.read()
    
    # Simple regex to find class definitions
    # It doesn't handle nested classes perfectly but should be enough for a scan
    classes = re.finditer(r'class\s+(\w+)(\s+final)?(\s+:\s+public\s+[\w:]+)?\s*\{', content)
    
    for match in classes:
        class_name = match.group(1)
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
        
        class_body = content[start_pos:end_pos-1]
        
        # Check if it has private or protected members
        if 'private:' in class_body or 'protected:' in class_body:
            continue
        
        # Check if it has virtual methods or many methods
        if 'virtual ' in class_body or 'override' in class_body:
            continue
            
        # Count methods (very rough estimate)
        method_count = class_body.count('(')
        # Exclude constructors/destructors roughly
        method_count -= class_body.count(class_name)
        
        # Count data members (very rough estimate)
        member_count = class_body.count(';')
        
        # If it has data members and few methods, it's a candidate
        if member_count > 0 and method_count <= 2:
             print(f"{file_path}: class {class_name} (methods: ~{method_count}, members: ~{member_count})")

for root, dirs, files in os.walk('.'):
    for file in files:
        if file.endswith('.h') or file.endswith('.hpp'):
            analyze_file(os.path.join(root, file))
