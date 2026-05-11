import re
import os
import sys

def refactor_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    # Pattern for std::lock_guard<std::mutex>
    # We want to replace it with std::lock_guard
    pattern = r'std::lock_guard<std::mutex>'
    replacement = 'std::lock_guard'
    
    new_content = re.sub(pattern, replacement, content)
    
    if new_content != content:
        with open(filepath, 'w') as f:
            f.write(new_content)
        return True
    return False

def main():
    root_dir = "."
    if len(sys.argv) > 1:
        root_dir = sys.argv[1]
        
    for root, dirs, files in os.walk(root_dir):
        if any(skip in root for skip in ['external', 'CMakeFiles', '.git', '.gemini']):
            continue
            
        for file in files:
            if file.endswith(('.cpp', '.h', '.hpp')):
                filepath = os.path.join(root, file)
                if refactor_file(filepath):
                    print(f"Refactored: {filepath}")

if __name__ == "__main__":
    main()
