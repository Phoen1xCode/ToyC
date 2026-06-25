#!/usr/bin/env python3
"""Tiny RV32IM user-mode simulator for this compiler's output.

Reads a RISC-V32 assembly file (as emitted by ./build/compiler), runs `main`
as the program entry, and returns `a0 & 0xff` as the process exit code.
Supports only the instruction subset our backend emits; anything else
prints an error and exits non-zero.

Usage: rv32im_sim.py <file.s> [max_steps]
"""
import sys

REGS = ['zero','ra','sp','gp','tp','t0','t1','t2',
        's0','s1','a0','a1','a2','a3','a4','a5','a6','a7',
        's2','s3','s4','s5','s6','s7','s8','s9','s10','s11',
        't3','t4','t5','t6']
REGI = {n:i for i,n in enumerate(REGS)}
ALIASES = {'fp':'s0','x0':'zero','x1':'ra','x2':'sp','x3':'gp','x4':'tp'}
for i in range(32):
    ALIASES[f'x{i}'] = REGS[i]

def reg_index(name):
    name = name.strip().lstrip('$')
    if name in REGI: return REGI[name]
    if name in ALIASES: return REGI[ALIASES[name]]
    raise ValueError(f"unknown register {name!r}")

def to_int(v):
    v &= 0xffffffff
    return v - 0x100000000 if v & 0x80000000 else v

def to_u32(v):
    return v & 0xffffffff

def parse_imm(s):
    s = s.strip()
    if s.startswith('-'):
        return -parse_imm(s[1:])
    if s.startswith('0x') or s.startswith('0X'):
        return int(s,16)
    try:
        return int(s)
    except ValueError:
        raise ValueError(f"bad immediate {s!r}")

class Sim:
    def __init__(self):
        self.r = [0]*32
        self.steps = 0
        self.mem = {}          # byte address -> value
        self.text = []         # list of (mnemonic, operands) labels already resolved to index
        self.labels = {}      # label -> instruction index (in text)
        self.globals = {}     # symbol -> address of next memory region
        self.data_bytes = bytearray()
        self.data_symbols = {}
        self.r[2] = 0x80000000 # sp near top of address space
        self.data_base_addr = 0x10000
        self.text_base_addr = 0x4000  # not used for addresses, only for understanding

    def store_byte(self, addr, val):
        self.mem[addr & 0xffffffff] = val & 0xff

    def store(self, addr, val):
        addr &= 0xffffffff
        for i in range(4):
            self.mem[(addr+i) & 0xffffffff] = (val >> (8*i)) & 0xff

    def load(self, addr):
        addr &= 0xffffffff
        return sum(self.mem.get((addr+i) & 0xffffffff, 0) << (8*i) for i in range(4))

    def load_byte(self, addr):
        return self.mem.get(addr & 0xffffffff, 0)

    # ---- assembler pass ----
    def assemble(self, src):
        lines = src.splitlines()
        # Collect data section first
        section = '.text'
        pending_labels = []
        text = []
        # Two passes:
        # Pass 1: gather instructions with placeholder label refs in operands;
        #         build labels mapping to instruction indices.
        # We treat directives specially: .data / .text switch sections;
        # .globl / .globl are ignored; .word emits data; labels recorded.
        for raw in lines:
            line = raw.split('#')[0].strip()
            if not line: continue
            # drop leading colon labels possibly inline
            # split labels prefix
            while ':' in line:
                lbl, _, rest = line.partition(':')
                lbl = lbl.strip()
                # data labels -> symbol addresses
                if section == '.data':
                    addr = self.data_base_addr + len(self.data_bytes)
                    self.data_symbols[lbl] = addr
                else:
                    self.labels[lbl] = len(text)
                line = rest.strip()
                if not line: break
            if not line: continue
            # directives
            if line.startswith('.'):
                parts = line.split(None, 1)
                d = parts[0]
                if d == '.data':
                    section = '.data'
                elif d == '.text':
                    section = '.text'
                elif d == '.globl' or d == '.global' or d == '.align' or d == '.globl':
                    pass
                elif d == '.word':
                    if section == '.data':
                        for tok in line[len('.word'):].split(','):
                            tok = tok.strip()
                            # store little-endian 4 bytes
                            if tok in self.data_symbols:
                                val = self.data_symbols[tok]
                            else:
                                try: val = int(tok, 0)
                                except: val = 0
                            for i in range(4):
                                self.data_bytes.append((val >> (8*i)) & 0xff)
                else:
                    pass
                continue
            if section != '.text':
                # strange; just skip
                continue
            # instruction
            mn, _, ops = line.partition(' ')
            ops = ops.strip()
            operands = [o.strip() for o in ops.split(',')] if ops else []
            text.append((mn, operands))
        self.text = text
        # Initialize mem with .data section bytes so that la+lw reads the
        # initial values of globals (without this, only sw-written globals
        # are observable; pure-read globals like `int g = 41; return g;`
        # would incorrectly load 0).
        for i, b in enumerate(self.data_bytes):
            self.mem[(self.data_base_addr + i) & 0xffffffff] = b

    def resolve_label(self, tok):
        tok = tok.strip()
        if tok in self.labels: return self.labels[tok]
        if tok in self.data_symbols: return self.data_symbols[tok]
        return None

    def mem_for(self, base_addr_expr):
        # for "la reg, symbol"; not used.
        pass

    def run(self, max_steps=50_000_000):
        if 'main' not in self.labels:
            raise RuntimeError("no main label")
        # set ra=0 to detect return from main; but main calls other functions which call ret (pop ra).
        # We model jal/jalr/call by pushing a return marker. Use a sentinel ra value 0 means "halt".
        pc = self.labels['main']
        r = self.r
        while True:
            self.steps += 1
            if self.steps > max_steps:
                raise RuntimeError("step limit exceeded (infinite loop?)")
            if pc < 0 or pc >= len(self.text):
                raise RuntimeError(f"pc out of range {pc}")
            mn, ops = self.text[pc]
            pc += 1
            # dispatch
            if mn in ('ret','jr','jr ra'):
                # use ra; if ra==0 -> program exit
                if r[1] == 0:
                    return to_int(r[10]) & 0xffffffff & 0xff
                pc = r[1] & 0xffffffff
                continue
            elif mn == 'jal' or mn == 'jalr':
                # jal rd, offset  OR jalrd rd, rs, 0 in our output? our backend uses `jal name`
                if len(ops) == 1:
                    target = self.resolve_label(ops[0])
                    if target is None: raise RuntimeError(f"unknown jal target {ops[0]}")
                    r[1] = pc  # save return
                    pc = target
                    continue
                else:
                    rd = reg_index(ops[0])
                    if mn == 'jal':
                        target = self.resolve_label(ops[1])
                        r[rd] = pc; pc = target; continue
                    else:
                        rs = reg_index(ops[1])
                        r[rd] = pc; pc = to_u32(r[rs]) + (parse_imm(ops[2]) if len(ops)>2 else 0); continue
            elif mn == 'j':
                target = self.resolve_label(ops[0])
                if target is None: raise RuntimeError(f"unknown j target {ops!r}")
                pc = target; continue
            elif mn == 'call':
                target = self.resolve_label(ops[0])
                r[1] = pc; pc = target; continue
            elif mn == 'li':
                r[reg_index(ops[0])] = to_u32(parse_imm(ops[1])); continue
            elif mn == 'mv':
                r[reg_index(ops[0])] = r[reg_index(ops[1])]; continue
            elif mn == 'la':
                t = ops[1]
                addr = self.resolve_label(t)
                if addr is None: raise RuntimeError(f"la unknown symbol {t}")
                r[reg_index(ops[0])] = to_u32(addr); continue
            elif mn == 'lui':
                r[reg_index(ops[0])] = to_u32(parse_imm(ops[1]) << 12); continue
            elif mn == 'auipc':
                # placeholder: ignore properly
                r[reg_index(ops[0])] = 0; continue
            elif mn == 'neg' or mn == 'negw':
                r[reg_index(ops[0])] = to_u32(-to_int(r[reg_index(ops[1])])); continue
            elif mn == 'not':
                r[reg_index(ops[0])] = to_u32(~to_int(r[reg_index(ops[1])])); continue
            elif mn == 'seqz':
                r[reg_index(ops[0])] = 1 if to_int(r[reg_index(ops[1])])==0 else 0; continue
            elif mn == 'snez':
                r[reg_index(ops[0])] = 0 if to_int(r[reg_index(ops[1])])==0 else 1; continue
            elif mn == 'mvnez':
                # pseudo: zero handling not implemented
                pass
            elif mn.endswith('i') and mn[:-1] in ('add','sub','sll','srl','sra','and','or','xor','slt','sltu'):
                op = mn[:-1]
                a = to_int(r[reg_index(ops[1])]); b = parse_imm(ops[2])
                res = self.alu(op, a, b)
                r[reg_index(ops[0])] = to_u32(res); continue
            elif mn in ('add','sub','mul','mulh','mulhu','div','divu','rem','remu',
                        'sll','srl','sra','and','or','xor','slt','sltu','mulw','divw','divuw','remw','remuw','sllw','srlw','sraw'):
                a = to_int(r[reg_index(ops[1])]); b = to_int(r[reg_index(ops[2])])
                res = self.alu_op(mn, a, b)
                r[reg_index(ops[0])] = to_u32(res); continue
            elif mn == 'sw':
                val = r[reg_index(ops[0])]
                base, off = self.split_mem(ops[1])
                self.store(to_u32(r[base]) + off, val); continue
            elif mn == 'sh':
                val = r[reg_index(ops[0])]
                base, off = self.split_mem(ops[1])
                for i in range(2): self.store_byte((to_u32(r[base])+off+i) & 0xffffffff, (val>>(8*i))&0xff)
                continue
            elif mn == 'sb':
                val = r[reg_index(ops[0])]
                base, off = self.split_mem(ops[1])
                self.store_byte(to_u32(r[base])+off, val); continue
            elif mn == 'lw':
                base, off = self.split_mem(ops[1])
                r[reg_index(ops[0])] = self.load(to_u32(r[base]) + off); continue
            elif mn == 'lwu':
                base, off = self.split_mem(ops[1])
                r[reg_index(ops[0])] = self.load(to_u32(r[base]) + off) & 0xffffffff; continue
            elif mn == 'lbu':
                base, off = self.split_mem(ops[1])
                r[reg_index(ops[0])] = self.load_byte(to_u32(r[base]) + off); continue
            elif mn == 'lb':
                base, off = self.split_mem(ops[1])
                v = self.load_byte(to_u32(r[base]) + off)
                if v & 0x80: v -= 256
                r[reg_index(ops[0])] = to_u32(v); continue
            elif mn == 'beqz':
                ra = reg_index(ops[0])
                if to_int(r[ra])==0: pc = self.resolve_label(ops[1]); 
                continue
            elif mn == 'bnez':
                ra = reg_index(ops[0])
                if to_int(r[ra])!=0: pc = self.resolve_label(ops[1])
                continue
            elif mn in ('beq','bne','blt','bge','bltu','bgeu','bgt','ble','bgtu','bleu'):
                ra = reg_index(ops[0]); rb = reg_index(ops[1])
                a = to_int(r[ra]); b = to_int(r[rb])
                take = False
                if mn=='beq': take = a==b
                elif mn=='bne': take = a!=b
                elif mn=='blt': take = a<b
                elif mn=='bge': take = a>=b
                elif mn=='bltu': take = (a & 0xffffffff) < (b & 0xffffffff)
                elif mn=='bgeu': take = (a & 0xffffffff) >= (b & 0xffffffff)
                elif mn=='bgt': take = a>b
                elif mn=='ble': take = a<=b
                elif mn=='bgtu': take = (a & 0xffffffff) > (b & 0xffffffff)
                elif mn=='bleu': take = (a & 0xffffffff) <= (b & 0xffffffff)
                if take: pc = self.resolve_label(ops[2])
                continue
            elif mn == 'fence' or mn == 'fence.i' or mn == 'ecall' or mn == 'ebreak':
                continue
            else:
                raise RuntimeError(f"unsupported instruction {mn!r} {ops!r} at step {self.steps} pc={pc-1}")

    def split_mem(self, tok):
        # offset(reg) -- parse offset and reg
        if '(' not in tok:
            # could be a label/symbol address used in lw/sw directly; we don't generate that here
            raise RuntimeError(f"unsupported mem operand {tok!r}")
        off, _, rest = tok.partition('(')
        off = off.strip()
        reg = rest.rstrip(')').strip()
        try:
            o = parse_imm(off) if off else 0
        except ValueError:
            o = 0
        return reg_index(reg), o

    def alu(self, op, a, b):
        if op=='add': return a+b
        if op=='sub': return a-b
        if op=='sll': return (a & 0xffffffff) << (b & 31)
        if op=='srl': return (a & 0xffffffff) >> (b & 31)
        if op=='sra': return to_int(a & 0xffffffff) >> (b & 31)
        if op=='and': return a & b
        if op=='or': return a | b
        if op=='xor': return a ^ b
        if op=='slt': return 1 if a < b else 0
        if op=='sltu': return 1 if (a & 0xffffffff) < (b & 0xffffffff) else 0

    def alu_op(self, mn, a, b):
        u = lambda x: x & 0xffffffff
        if mn=='add': return a+b
        if mn=='sub': return a-b
        if mn=='mul': return to_int(u(a)*u(b))
        if mn=='mulh':
            # signed × signed, high 32 bits. `a` and `b` are already
            # signed Python ints (to_int normalises).
            return (a * b) >> 32
        if mn=='mulhu': return (u(a)*u(b)) >> 32
        if mn=='mulhsu':
            # signed × unsigned, high 32 bits.
            return (a * u(b)) >> 32
        if mn=='mulw': return to_int(u(a)*u(b))
        if mn=='div': return self.idiv(a,b)
        if mn=='divu': return u(a)//u(b) if b else -1
        if mn=='divw': return self.idiv(a,b)
        if mn=='divuw': return u(a)//u(b) if b else -1
        if mn=='rem': return self.irem(a,b)
        if mn=='remu': return u(a)%u(b) if b else u(a)
        if mn=='remw': return self.irem(a,b)
        if mn=='remuw': return u(a)%u(b) if b else u(a)
        if mn=='sll': return (u(a) << (b & 31))
        if mn=='srl': return u(a) >> (b & 31)
        if mn=='sra': return to_int(u(a)) >> (b & 31)
        if mn=='and': return a & b
        if mn=='or': return a | b
        if mn=='xor': return a ^ b
        if mn=='slt': return 1 if a<b else 0
        if mn=='sltu': return 1 if u(a)<u(b) else 0
        if mn=='sllw': return (u(a) << (b & 31))
        if mn=='srlw': return u(a) >> (b & 31)
        if mn=='sraw': return to_int(u(a)) >> (b & 31)
        return 0

    def idiv(self,a,b):
        if b==0: return -1
        # RISC-V div truncates toward zero
        q = abs(a)//abs(b)
        return q if (a<0)==(b<0) else -q

    def irem(self,a,b):
        if b==0: return a
        r = abs(a)%abs(b)
        return r if a>=0 else -r

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    path = sys.argv[1]
    max_steps = int(sys.argv[2]) if len(sys.argv) > 2 else 50_000_000
    with open(path) as f:
        src = f.read()
    sim = Sim()
    sim.assemble(src)
    try:
        code = sim.run(max_steps)
    except Exception as e:
        sys.stderr.write(f"sim error: {e}\n")
        sys.exit(254)
    sys.stderr.write(f"STEPS:{sim.steps}\n")
    sys.exit(code)

if __name__ == '__main__':
    main()