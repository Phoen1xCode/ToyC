#!/usr/bin/env python3
"""Run performance benchmarks against ./build/compiler.

For each tests/perf/pXX.tc:
  1. Reference: compile it as C with gcc -O2, run, capture exit code.
     (ToyC is a valid C subset.)
  2. Compiler:  ./build/compiler < pXX.tc > out.s
  3. Simulate the generated assembly, capture exit code and step count.
  4. Report PASS/FAIL plus step count (lower = faster).

Usage: tools/run_perf.py [--also-opt]
"""
import os, subprocess, sys, glob, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.path.join(ROOT, 'build', 'compiler')
SIM = os.path.join(ROOT, 'tools', 'rv32im_sim.py')
PERF = os.path.join(ROOT, 'tests', 'perf')

def run(cmd, stdin=None, timeout=60):
    return subprocess.run(cmd, input=stdin, capture_output=True,
                          text=True, timeout=timeout)

def gcc_exit(path):
    src = open(path).read()
    elf = '/tmp/_ref_elf'
    # toyC is C subset: compile as C
    p = subprocess.run(['gcc','-O2','-x','c','-o',elf,'-'],
                       input=src, capture_output=True, text=True)
    if p.returncode != 0:
        return None, p.stderr
    r = subprocess.run([elf], capture_output=True)
    return r.returncode & 0xff, None

def ours(path, use_opt, sim_steps=80_000_000):
    src = open(path).read()
    cmd = [CC] + (['-opt'] if use_opt else [])
    try:
        p = subprocess.run(cmd, input=src, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return None, None, None, 'compile-timeout'
    if p.returncode != 0:
        return None, None, None, f"cc-exit={p.returncode}:{p.stderr.strip()[:120]}"
    asm = p.stdout
    with open('/tmp/_out.s','w') as f: f.write(asm)
    try:
        r = subprocess.run(['python3', SIM, '/tmp/_out.s', str(sim_steps)],
                           capture_output=True, timeout=180)
    except subprocess.TimeoutExpired:
        return None, len(asm.splitlines()), None, 'sim-timeout'
    if r.returncode == 254:
        # simulator runtime error
        return None, len(asm.splitlines()), None, f"sim-err:{r.stderr.decode(errors='replace').strip()[:120]}"
    steps = None
    errTxt = r.stderr.decode(errors='replace')
    import re as _re
    m = _re.search(r'STEPS:(\d+)', errTxt)
    if m: steps = int(m.group(1))
    return r.returncode, len(asm.splitlines()), steps, 'OK'

def main():
    use_opt = '--also-opt' in sys.argv
    files = sorted(glob.glob(os.path.join(PERF, 'p*.tc')))
    if len(sys.argv) > 1 and not sys.argv[1].startswith('--'):
        files = [os.path.join(PERF, f) for f in sys.argv[1:]]
    print(f"{'name':<22}{'refexit':>8}{'got':>6}{'status':>14}{'asm#':>9}{'steps':>14}")
    print('-'*74)
    npass = n = 0
    for f in files:
        n += 1
        ref, err = gcc_exit(f)
        if ref is None:
            print(f"{os.path.basename(f):<22}{'-':>8}{'-':>6}{'gccFAIL':>14}{'-':>9}{'-':>12}  {err[:80]}")
            continue
        res = ours(f, use_opt)
        got, asmcount, steps, status = res
        ok = (got == ref) and status == 'OK'
        if ok: npass += 1
        marker = 'PASS' if ok else 'FAIL'
        steps_str = f"{steps}" if steps is not None else '-'
        print(f"{os.path.basename(f):<22}{ref:>8}{str(got):>6}{marker:>14}{(asmcount or 0):>9}{steps_str:>14}")
        if not ok:
            print(f"   -> {os.path.basename(f)} status={status} ref={ref} got={got}")
    print('-'*72)
    print(f"PASS {npass}/{n}")

if __name__ == '__main__':
    main()