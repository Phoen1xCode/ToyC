#!/usr/bin/env python3
"""Functional regression: run lv7/lv8 ToyC-compatible tests + check exit codes."""
import os, subprocess, sys, glob, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.path.join(ROOT, 'build', 'compiler')
SIM = os.path.join(ROOT, 'tools', 'rv32im_sim.py')
PUBLIC = os.path.join(ROOT, 'github', 'compiler-dev-test-cases', 'testcases')

def is_toyc_compat(src):
    if 'putint' in src or 'putch' in src or 'getint' in src or 'getch' in src:
        return False
    # skip arrays
    if re.search(r'\bint\s+\w+\s*\[', src):
        return False
    return True

def gcc_exit(src):
    elf = '/tmp/_ref_elf'
    try:
        p = subprocess.run(['gcc','-O2','-x','c','-o',elf,'-'],
                           input=src, capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return None
    if p.returncode != 0:
        return None
    try:
        r = subprocess.run([elf], timeout=5)
        return r.returncode & 0xff
    except subprocess.TimeoutExpired:
        return None

def ours(src):
    p = subprocess.run([CC], input=src, capture_output=True, text=True, timeout=20)
    if p.returncode != 0:
        return None, f"cc-exit={p.returncode}"
    asm = p.stdout
    with open('/tmp/_out.s','w') as f: f.write(asm)
    try:
        r = subprocess.run(['python3', SIM, '/tmp/_out.s', '100000000'],
                           capture_output=True, timeout=30)
        return r.returncode & 0xff, 'OK'
    except subprocess.TimeoutExpired:
        return None, 'sim-timeout'

def main():
    files = []
    for d in ['lv7', 'lv8']:
        for f in sorted(glob.glob(os.path.join(PUBLIC, d, '*.c'))):
            src = open(f).read()
            if is_toyc_compat(src):
                files.append((os.path.relpath(f, ROOT), src))
    print(f"running {len(files)} functional tests...")
    npass = 0
    fails = []
    for name, src in files:
        ref = gcc_exit(src)
        if ref is None:
            print(f"  {name}: SKIP (gcc failed)")
            continue
        got, status = ours(src)
        ok = (got == ref) and status == 'OK'
        if ok:
            npass += 1
            print(f"  {name}: PASS (exit={got})")
        else:
            fails.append((name, ref, got, status))
            print(f"  {name}: FAIL ref={ref} got={got} status={status}")
    print(f"\nPASS {npass}/{len(files)}")
    if fails:
        print("FAILURES:")
        for n, r, g, s in fails:
            print(f"  {n}: ref={r} got={g} status={s}")
        sys.exit(1)

if __name__ == '__main__':
    main()
