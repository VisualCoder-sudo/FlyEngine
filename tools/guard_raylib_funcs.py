#!/usr/bin/env python3
"""Wrap selected top-level function definitions of a raylib C module in
`#if !defined(RL_SOKOL_BACKEND)` so FlyEngine's sokol layer can provide them.
Used once when vendoring raylib modules into src/Engine/RL/raylib (kept for
re-vendoring)."""
import re, sys

path, names = sys.argv[1], sys.argv[2:]
lines = open(path).read().split('\n')
out, i, wrapped = [], 0, set()
sig = re.compile(r'^[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(')
while i < len(lines):
    m = sig.match(lines[i])
    if m and m.group(1) in names and not lines[i].rstrip().endswith(';'):
        # include preceding comment block
        j = len(out)
        while j > 0 and out[j-1].startswith('//'):
            j -= 1
        out.insert(j, '#if !defined(RL_SOKOL_BACKEND)')
        while True:
            out.append(lines[i])
            if lines[i].startswith('}'):
                break
            i += 1
        out.append('#endif // !RL_SOKOL_BACKEND')
        wrapped.add(m.group(1))
        i += 1
        continue
    out.append(lines[i]); i += 1
missing = set(names) - wrapped
if missing:
    sys.exit(f"{path}: not found: {sorted(missing)}")
open(path, 'w').write('\n'.join(out))
print(f"{path}: guarded {len(wrapped)} functions")
