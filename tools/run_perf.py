#!/usr/bin/env python3
"""Run performance benchmarks against ./build/compiler.

For each tests/perf/pXX.tc:
  1. Reference: compile it as C with gcc -O2, run, capture exit code.
     (ToyC is a valid C subset.)
  2. Compiler:  ./build/compiler < pXX.tc > out.s
  3. Simulate the generated assembly, capture exit code and step count.
  4. Report PASS/FAIL plus step count (lower = faster) and wall time.

Per-test caps (override via env vars):
  PERF_SIM_STEPS    max simulated instructions (default 200_000_000)
  PERF_SIM_TIMEOUT  simulator wall-clock seconds (default 30)
  PERF_CC_TIMEOUT   compiler wall-clock seconds  (default 30)
  PERF_GCC_TIMEOUT  gcc reference timeout        (default 15)

Usage: tools/run_perf.py [--also-opt] [pXX.tc ...]
"""
import os, subprocess, sys, glob, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.path.join(ROOT, 'build', 'compiler')
SIM = os.path.join(ROOT, 'tools', 'rv32im_sim.py')
PERF = os.path.join(ROOT, 'tests', 'perf')

SIM_STEPS    = int(os.environ.get('PERF_SIM_STEPS', 100_000_000))
SIM_TIMEOUT  = float(os.environ.get('PERF_SIM_TIMEOUT', 30))
CC_TIMEOUT   = float(os.environ.get('PERF_CC_TIMEOUT', 20))
GCC_TIMEOUT  = float(os.environ.get('PERF_GCC_TIMEOUT', 10))

def gcc_exit(path):
    src = open(path).read()
    elf = '/tmp/_ref_elf'
    try:
        p = subprocess.run(['gcc','-O2','-x','c','-o',elf,'-'],
                           input=src, capture_output=True, text=True,
                           timeout=GCC_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, 'gcc-compile-timeout'
    if p.returncode != 0:
        return None, p.stderr
    try:
        r = subprocess.run([elf], capture_output=True, timeout=GCC_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, 'gcc-run-timeout'
    return r.returncode & 0xff, None

def ours(path, use_opt):
    src = open(path).read()
    cmd = [CC] + (['-opt'] if use_opt else [])
    try:
        p = subprocess.run(cmd, input=src, capture_output=True, text=True,
                           timeout=CC_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, None, None, None, 'compile-timeout'
    if p.returncode != 0:
        return None, None, None, None, f"cc-exit={p.returncode}:{p.stderr.strip()[:120]}"
    asm = p.stdout
    with open('/tmp/_out.s','w') as f: f.write(asm)
    t0 = time.monotonic()
    try:
        r = subprocess.run(['python3', SIM, '/tmp/_out.s', str(SIM_STEPS)],
                           capture_output=True, timeout=SIM_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, len(asm.splitlines()), None, SIM_TIMEOUT, 'sim-timeout'
    dt = time.monotonic() - t0
    if r.returncode == 254:
        return None, len(asm.splitlines()), None, dt, f"sim-err:{r.stderr.decode(errors='replace').strip()[:120]}"
    steps = None
    errTxt = r.stderr.decode(errors='replace')
    import re as _re
    m = _re.search(r'STEPS:(\d+)', errTxt)
    if m: steps = int(m.group(1))
    return r.returncode, len(asm.splitlines()), steps, dt, 'OK'

def main():
    use_opt = '--also-opt' in sys.argv
    files = sorted(glob.glob(os.path.join(PERF, 'p*.tc')))
    extra = [a for a in sys.argv[1:] if not a.startswith('--')]
    if extra:
        files = [a if os.path.isabs(a) else os.path.join(PERF, a) for a in extra]
    print(f"caps: steps<= {SIM_STEPS:,}  sim_timeout={SIM_TIMEOUT}s  cc_timeout={CC_TIMEOUT}s")
    print(f"{'name':<24}{'refexit':>8}{'got':>6}{'status':>12}{'asm#':>8}{'steps':>14}{'sec':>8}")
    print('-'*84)
    npass = n = 0
    for f in files:
        n += 1
        ref, err = gcc_exit(f)
        if ref is None:
            print(f"{os.path.basename(f):<24}{'-':>8}{'-':>6}{'gccFAIL':>12}{'-':>8}{'-':>14}{'-':>8}  {(err or '')[:80]}")
            continue
        got, asmcount, steps, dt, status = ours(f, use_opt)
        ok = (got == ref) and status == 'OK'
        if ok: npass += 1
        marker = 'PASS' if ok else 'FAIL'
        steps_str = f"{steps:,}" if steps is not None else '-'
        dt_str = f"{dt:.2f}" if dt is not None else '-'
        print(f"{os.path.basename(f):<24}{ref:>8}{str(got):>6}{marker:>12}{(asmcount or 0):>8}{steps_str:>14}{dt_str:>8}")
        if not ok:
            print(f"   -> {os.path.basename(f)} status={status} ref={ref} got={got}")
    print('-'*84)
    print(f"PASS {npass}/{n}")

if __name__ == '__main__':
    main()
