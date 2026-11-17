#!/usr/bin/env python3
"""Check that every local image referenced by a markdown file exists."""
import os
import re
import sys

ROOT = os.getcwd()
IMG = re.compile(r'!\[[^\]]*\]\(([^)\s]+)')

def check(md_path):
    base = os.path.dirname(md_path)
    txt = open(md_path, encoding='utf-8', errors='replace').read()
    bad = []
    total = 0
    for m in IMG.finditer(txt):
        rel = m.group(1)
        if rel.startswith(('http://', 'https://')):
            continue
        total += 1
        p = os.path.normpath(os.path.join(base, rel))
        if not os.path.exists(p):
            bad.append((rel, p))
    print('%-52s images=%d missing=%d' % (os.path.relpath(md_path, ROOT), total, len(bad)))
    for rel, p in bad:
        print('    MISSING %s -> %s' % (rel, p))
    return bad

targets = sys.argv[1:]
if not targets:
    targets = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames
                       if d not in ('imgui', 'build', 'build_x64', 'build_x86',
                                    'build_dll64', '_ai_tmp', 'HalconDemo',
                                    'release_GeoGUIDemo_x64', '_sdk64', '_sdk64dll', '.git')]
        for f in filenames:
            if f.endswith('.md'):
                targets.append(os.path.join(dirpath, f))

fails = 0
for t in targets:
    fails += len(check(t))
print('TOTAL MISSING =', fails)
