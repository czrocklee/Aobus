import re
import os

filepath = "lib/audio/backend/PipeWireBackend.cpp"

with open(filepath, 'r') as f:
    content = f.read()

# Members list from struct Impl
members = [
    "renderTarget", "format", "drainPending", "strictFormatRequired", 
    "strictFormatRejected", "routeAnchorReported", "threadLoop", 
    "context", "core", "stream", "streamListener", "volume", 
    "muted", "volumeAvailable"
]

new_content = content

# Replace _impl->_member with _impl->member
for member in members:
    new_content = new_content.replace(f"_impl->_{member}", f"_impl->{member}")
    # Replace member definition and usage within Impl methods
    # We look for _member but be careful not to match partial words
    new_content = re.sub(rf'\b_{member}\b', member, new_content)

if new_content != content:
    with open(filepath, 'w') as f:
        f.write(new_content)
    print(f"Refactored: {filepath}")
else:
    print(f"No changes in {filepath}")
