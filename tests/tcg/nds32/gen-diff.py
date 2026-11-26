#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate an NDS32 instruction test.

Each group runs many instances of an instruction class with interesting
operands and stores every result to memory at 0x400000 (r10 is the result
pointer).  The generated program folds the results into a checksum and
reports pass or fail through the nds32-virt test finisher when a reference
checksum is given with --expect.  The reference checksums in
diff-expected.txt come from the instruction set simulator of the Andes gdb:
see oracle-digests.py.
"""
import sys
import random

random.seed(1)
V = [0, 1, 2, 5, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x12345678,
     0x7fffffff, 0x80000000, 0x80000001, 0xdeadbeef, 0xffffffff, 0xfffffffe,
     31, 32, 33]
VS = [0, 1, 0x7f, 0x80000000, 0xffffffff, 0x12345678, 31, 0xdeadbeef, 3]
out = []
def e(s): out.append('\t' + s)
def li(r, v): e('sethi %s,%d' % (r, (v >> 12) & 0xfffff)); e('ori %s,%s,%d' % (r, r, v & 0xfff))
CNT=[0]
DESC=[]
CUR=['']
def save(*regs):
    CNT[0]+=len(regs)
    for r in regs: DESC.append(CUR[0])
    for r in regs: e('swi.bi %s,[$r10],4' % r)

def binop(fmt, vals=V, outs=('$r3',), pre=None, skipb0=False):
    for a in vals:
        for b in vals:
            if skipb0 and b == 0: continue
            if skipb0 and a == 0x80000000 and b == 0xffffffff: continue
            li('$r1', a); li('$r2', b)
            li('$r3', 0x55aa55aa); li('$r4', 0x33cc33cc)
            if pre: e(pre)
            CUR[0] = '%s a=%#x b=%#x' % (fmt.split()[0], a, b)
            e(fmt)
            save(*outs)

hdr = """\t.text
\t.globl _start
_start:
\tsethi $r10, 0x400
\tsethi $sp, 0x600
"""
body_groups = {}
GC = {}
_cur = [None, 0]
def group(name):
    global out
    if _cur[0]:
        GC[_cur[0]] = CNT[0] - _cur[1]
    _cur[0], _cur[1] = name, CNT[0]
    out = []
    body_groups[name] = out

group('alu')
for op in ['add','sub','and','xor','or','nor','slt','slts','sll','srl','sra','rotr','bitc','max','min','ave','sva','svs','mul','cmovz','cmovn']:
    binop('%s $r3,$r1,$r2' % op)
for op in ['add_slli','sub_slli','and_slli','xor_slli','or_slli','add_srli','sub_srli','and_srli','xor_srli','or_srli']:
    for sh in (0, 1, 7, 31):
        binop('%s $r3,$r1,$r2,%d' % (op, sh), vals=VS)
for op in ['seb','seh','zeh','wsbh','abs','clz','clo','zeb','neg']:
    binop('%s $r3,$r1' % op, vals=V)
group('imm')
for op in ['slli','srli','srai','rotri','clips','clip','bset','bclr','btgl','btst']:
    for imm in (0, 1, 5, 15, 31):
        binop('%s $r3,$r1,%d' % (op, imm), vals=VS)
for op in ['addi','subri','slti','sltsi']:
    for imm in (0, 1, -1, 0x3fff, -0x4000, 100):
        binop('%s $r3,$r1,%d' % (op, imm), vals=VS)
for op in ['andi','xori','ori','bitci']:
    for imm in (0, 1, 0x7fff, 0x5555, 0xff):
        binop('%s $r3,$r1,%d' % (op, imm), vals=VS)
for imm in (0, 1, -1, 0x7ffff, -0x80000, 1234):
    li('$r3', 0x77777777); e('movi $r3,%d' % imm); save('$r3')
for imm in (0, 1, 0xfffff, 0x12345):
    e('sethi $r3,%d' % imm); save('$r3')
group('mul')
for op in ['mults64','mult64','madds64','madd64','msubs64','msub64','mult32','madd32','msub32']:
    for d in ('$d0', '$d1'):
        for a in VS:
            for b in VS[:6]:
                li('$r1', a); li('$r2', b)
                li('$r5', 0x01234567); li('$r6', 0x89abcdef)
                e('mtusr $r5,%s.lo' % d); e('mtusr $r6,%s.hi' % d)
                e('%s %s,$r1,$r2' % (op, d))
                e('mfusr $r3,%s.lo' % d); e('mfusr $r4,%s.hi' % d)
                save('$r3', '$r4')
for op in ['mulr64','mulsr64']:
    for a in VS:
        for b in VS[:6]:
            for rt in ('$r12', '$r13'):
                li('$r1', a); li('$r2', b); li('$r12', 0x1111); li('$r13', 0x2222)
                e('%s %s,$r1,$r2' % (op, rt)); save('$r12', '$r13')
for op in ['maddr32','msubr32']:
    binop('%s $r3,$r1,$r2' % op, vals=VS)
group('div')
for op, rt in [('divr', 1), ('divsr', 1)]:
    binop('%s $r3,$r4,$r1,$r2' % op, vals=V, outs=('$r3','$r4'), skipb0=True)
for op in ['div','divs']:
    binop('%s $d0,$r1,$r2\n\tmfusr $r3,$d0.lo\n\tmfusr $r4,$d0.hi' % op, outs=('$r3','$r4'), skipb0=True)

def raw(line): e(line)
def lab(): 
    lab.n += 1
    return 'L%d' % lab.n
lab.n = 0

group('mem')
e('sethi $r11, 0x500')   # buffer at 0x500000 (data)
e('sethi $r12, 0x500')
# fill buffer with a pattern
for i in range(16):
    li('$r1', (0x01020304 * (i + 1)) ^ 0xa5a5a5a5 ^ (i << 24)); e('swi $r1,[$r11+%d]' % (i * 4))
CUR[0] = 'ld'
for op, offs in [('lbi',(0,1,2,3,7,-1,-4)),('lhi',(0,2,4,-2,6)),('lwi',(0,4,8,-4,12)),('lbsi',(0,1,2,3,5)),('lhsi',(0,2,4,6))]:
    for o in offs:
        e('addi $r11,$r12,16'); li('$r3',0x1111); e('%s $r3,[$r11+(%d)]' % (op, o)); save('$r3')
for op, d in [('lbi.bi',(1,-1,3)),('lhi.bi',(2,-2,6)),('lwi.bi',(4,-4,8)),('lbsi.bi',(1,2)),('lhsi.bi',(2,4))]:
    for o in d:
        e('addi $r11,$r12,16'); e('%s $r3,[$r11],%d' % (op, o)); save('$r3', '$r11')
for op, sc in [('sbi',1),('shi',2),('swi',4)]:
    for o in (0, 1, 2, 4, 8, -4):
        li('$r5', 0xcafebabe)
        for i in range(8): e('swi $r5,[$r12+%d]' % (16 + i * 4 + 8 if False else i * 4))
        e('addi $r11,$r12,16'); li('$r4', 0x89abcdef); e('%s $r4,[$r11+(%d)]' % (op, o if o % sc == 0 else 0))
        for i in range(-3, 7): e('lwi $r3,[$r12+%d]' % (16 + i * 4)); save('$r3')
for op, sc in [('sbi.bi',1),('shi.bi',2),('swi.bi',4)]:
    for o in (sc, -sc, 2*sc):
        e('addi $r11,$r12,32'); li('$r4', 0x76543210); e('%s $r4,[$r11],%d' % (op, o)); save('$r11')
        for i in range(-2, 6): e('lwi $r3,[$r12+%d]' % (32 + i * 4)); save('$r3')
for op in ['lb','lh','lw','lbs','lhs']:
    for sv in (0, 1, 2, 3):
        e('addi $r11,$r12,0'); li('$r2', 4 if sv != 3 else 2); e('%s $r3,[$r11+$r2<<%d]' % (op, sv)); save('$r3')
for op in ['lb.bi','lh.bi','lw.bi','lbs.bi','lhs.bi']:
    for sv in (0, 1, 2):
        e('addi $r11,$r12,16'); li('$r2', 4); e('%s $r3,[$r11],$r2<<%d' % (op, sv)); save('$r3', '$r11')
for op in ['sb','sh','sw']:
    for sv in (0, 1, 2):
        e('addi $r11,$r12,16'); li('$r2', 4); li('$r4', 0x0badf00d); e('%s $r4,[$r11+$r2<<%d]' % (op, sv))
        for i in range(0, 9): e('lwi $r3,[$r12+%d]' % (16 + i * 4)); save('$r3')
for op in ['sb.bi','sh.bi','sw.bi']:
    e('addi $r11,$r12,16'); li('$r2', 4); li('$r4', 0x0c0ffee0); e('%s $r4,[$r11],$r2<<1' % op); save('$r11')
    for i in range(0, 9): e('lwi $r3,[$r12+%d]' % (16 + i * 4)); save('$r3')
# gp relative
e('addi $gp,$r12,64')
for op, offs in [('lbi.gp',(0,5,-3)),('lbsi.gp',(1,6)),('lhi.gp',(0,2,6,-2)),('lhsi.gp',(0,4)),('lwi.gp',(0,4,8,-8))]:
    for o in offs: e('%s $r3,[+%d]' % (op, o)); save('$r3')
for op, offs in [('sbi.gp',(0,5)),('shi.gp',(0,6)),('swi.gp',(0,8))]:
    for o in offs:
        li('$r4', 0x5a5a1234); e('%s $r4,[+%d]' % (op, o))
        for i in range(0, 6): e('lwi $r3,[$r12+%d]' % (64 + i * 4)); save('$r3')
e('addi.gp $r3,10'); save('$r3')
# 16-bit forms
for op, sc in [('lwi333',4),('lhi333',2),('lbi333',1)]:
    for o in range(0, 8):
        e('addi $r1,$r12,16'); e('%s $r2,[$r1+(%d)]' % (op, o * sc)); save('$r2')
for o in range(0, 8):
    e('addi $r1,$r12,16'); e('lwi333.bi $r2,[$r1],%d' % (o * 4)); save('$r2', '$r1')
for op, sc in [('swi333',4),('shi333',2),('sbi333',1)]:
    for o in range(0, 8, 3):
        e('addi $r1,$r12,16'); li('$r2', 0x13572468); e('%s $r2,[$r1+(%d)]' % (op, o * sc))
        for i in range(0, 8): e('lwi $r3,[$r12+%d]' % (16 + i * 4)); save('$r3')
e('addi $r1,$r12,16'); li('$r2', 0x24681357); e('swi333.bi $r2,[$r1],8'); save('$r1')
e('addi $r1,$r12,16'); e('lwi450 $r2,[$r1]'); save('$r2')
e('addi $r1,$r12,16'); li('$r2', 0xdead0001); e('swi450 $r2,[$r1]'); e('lwi $r3,[$r1+0]'); save('$r3')
e('addi $fp,$r12,16')
for o in (0, 4, 124): e('lwi37 $r2,[$fp+%d]' % o); save('$r2')
for o in (0, 8): li('$r2', 0xabc00 + o); e('swi37 $r2,[$fp+%d]' % o); e('lwi $r3,[$fp+%d]' % o); save('$r3')
e('addi $sp,$r12,16')
for o in (0, 4, 20): e('lwi37.sp $r2,[+%d]' % o); save('$r2')
for o in (0, 12): li('$r2', 0xdef00 + o); e('swi37.sp $r2,[+%d]' % o); e('lwi $r3,[$sp+%d]' % o); save('$r3')
e('addi $r8,$r12,64')
for o in (-128, -64, -4): e('lwi45.fe $r2,[%d]' % o); save('$r2')
e('sethi $sp, 0x600')
group('br')
conds2 = ['beq','bne']
for a in VS:
    for b in (0, 1, 0x7f, 0xffffffff, 0x80000000):
        for op in ['beq','bne','beqc','bnec','beqs38','bnes38']:
            L = lab()
            li('$r1', a); li('$r2', b); li('$r5', b); li('$r9', a); e('movi $r3,0')
            CUR[0] = '%s a=%#x b=%#x' % (op, a, b)
            if op == 'beq': e('beq $r9,$r2,%s' % L)
            elif op == 'bne': e('bne $r9,$r2,%s' % L)
            elif op == 'beqc': e('beqc $r9,%d,%s' % (b & 0x3ff if b in (0,1,0x7f) else 5, L))
            elif op == 'bnec': e('bnec $r9,%d,%s' % (b & 0x3ff if b in (0,1,0x7f) else 5, L))
            elif op == 'beqs38': e('beqs38 $r1,%s' % L)
            else: e('bnes38 $r1,%s' % L)
            e('movi $r3,1'); e('%s:' % L); save('$r3')
for a in VS:
    for op in ['beqz','bnez','bgez','bltz','bgtz','blez','beqz38','bnez38','beqzs8','bnezs8']:
        L = lab()
        li('$r1', a); li('$r9', a); e('movi $r3,0'); li('$r15', a)
        CUR[0] = '%s a=%#x' % (op, a)
        if op.endswith('38'): e('%s $r1,%s' % (op[:-2] + '38', L))
        elif op.endswith('s8'): e('%s %s' % (op, L))
        else: e('%s $r9,%s' % (op, L))
        e('movi $r3,1'); e('%s:' % L); save('$r3')
for a in VS:
    for op in ['bgezal','bltzal']:
        L = lab()
        li('$r9', a); e('movi $lp,7'); e('%s $r9,%s' % (op, L)); e('nop'); e('%s:' % L); e('mov55 $r3,$lp'); save('$r3')
# calls and register jumps
L1, L2, L3, L4 = lab(), lab(), lab(), lab()
e('jal %s' % L1); e('movi $r3,1'); e('j %s' % L2); e('%s:' % L1); e('mov55 $r4,$lp'); e('ret'); e('%s:' % L2); save('$r3', '$r4')
e('la $r5,%s' % L3); e('jral $r6,$r5'); e('movi $r3,2'); e('%s:' % L3); save('$r6')
L5 = lab(); L6 = lab()
e('la $r5,%s' % L5); e('jral5 $r5'); e('movi $r3,3'); e('j %s' % L6); e('%s:' % L5); e('mov55 $r4,$lp'); e('ret5 $lp'); e('%s:' % L6); save('$r3', '$r4')
L7 = lab(); e('la $r5,%s' % L7); e('jr5 $r5'); e('movi $r3,4'); e('%s:' % L7); e('nop')
L8 = lab(); L9 = lab()
e('movi $r5,0'); e('la $r6,%s' % L8); e('jrnez $r5'); e('movi $r3,5'); e('%s:' % L8); save('$r3')
e('la $r5,%s' % L9); e('movi $r6,0x33'); e('jralnez $r6,$r5'); e('movi $r3,6'); e('%s:' % L9); e('nop'); save('$r6')
group('lsmw')
e('sethi $r20, 0x500')
for form in ['bi','bim','bd','bdm','ai','aim','ad','adm']:
    for (rb, re, en) in [(0,2,0),(5,5,0),(6,9,0xe),(3,3,0x1),(31,31,0x2),(31,31,0xe),(7,8,0xf),(8,9,0x8)]:
        for st in (0, 1):
            for i in range(-8, 24):
                li('$r21', 0x1000 + i * 0x111); e('swi $r21,[$r20+%d]' % (64 + i * 4))
            for i in range(0, 10): li('$r%d' % i, 0x2000 + i)
            li('$fp', 0x2100); li('$gp', 0x2200); li('$lp', 0x2300)
            e('addi $r13,$r20,64')
            CUR[0] = '%s.%s %d %d %x' % ('smw' if st else 'lmw', form, rb, re, en)
            nm = ('smw' if st else 'lmw') + '.' + form
            e('%s $r%d,[$r13],$r%d,%d' % (nm, rb, re, en))
            save('$r13', '$fp', '$gp', '$lp')
            for i in range(0, 10): save('$r%d' % i)
            for i in range(-6, 14): e('lwi $r3,[$r20+%d]' % (64 + i * 4)); save('$r3')
e('sethi $sp, 0x600')
group('stack')
PTR = ['$r10']
def save(*regs):
    CNT[0] += len(regs)
    for r in regs:
        DESC.append(CUR[0])
        e('swi.bi %s,[%s],4' % (r, PTR[0]))
e('mov55 $r20,$r10')
PTR[0] = '$r20'
for re in ('$r6', '$r8', '$r10', '$r14'):
    for imm in (0, 8, 16, 248):
        CUR[0] = 'push25/pop25 %s %d' % (re, imm)
        e('sethi $r12, 0x500'); e('addi $sp,$r12,640')
        for i in (6, 7, 8, 9, 10, 11, 12, 13, 14): li('$r%d' % i, 0x7000 + i)
        li('$fp', 0xf0f0f0f0); li('$gp', 0x9999); li('$lp', 0x3000c)
        e('push25 %s,%d' % (re, imm)); save('$sp')
        for i in range(-12, 4): e('lwi $r3,[$sp+%d]' % (imm + i * 4)); save('$r3')
        for i in (6, 7, 8, 9, 10, 11, 12, 13, 14): e('movi $r%d,0' % i)
        e('movi $fp,0'); e('movi $gp,0'); L = lab(); e('la $lp,%s' % L)
        n = {'$r6': 4, '$r8': 6, '$r10': 8, '$r14': 12}[re]
        e('swi $lp,[$sp+%d]' % (imm + 4 * (n - 1)))
        e('pop25 %s,%d' % (re, imm)); e('movi $r3,99'); e('%s:' % L)
        save('$sp', '$fp', '$gp', '$lp', '$r6', '$r7', '$r8', '$r9', '$r10', '$r14', '$r3')
e('mov55 $r10,$r20')
PTR[0] = '$r10'
e('sethi $sp, 0x600')
group('misc16')
for op in ['add45','sub45','slts45','slt45']:
    for a in VS:
        for b in VS[:6]:
            li('$r1', a); li('$r2', b); li('$r15', 0x77)
            CUR[0] = '%s a=%#x b=%#x' % (op, a, b)
            e('%s $r1,$r2' % op); save('$r1', '$r15')
for op in ['addi45','subi45','srai45','srli45','sltsi45','slti45']:
    for a in VS:
        for i in (0, 1, 5, 31):
            li('$r1', a); li('$r15', 0x77); CUR[0] = '%s a=%#x b=%d' % (op, a, i)
            e('%s $r1,%d' % (op, i)); save('$r1', '$r15')
for op in ['add333','sub333']:
    for a in VS:
        for b in VS[:5]:
            li('$r1', a); li('$r2', b); e('%s $r3,$r1,$r2' % op); save('$r3')
for op in ['slli333','addi333','subi333']:
    for a in VS:
        for i in range(8):
            li('$r1', a); e('%s $r3,$r1,%d' % (op, i)); save('$r3')
for op in ['zeb33','zeh33','seb33','seh33','xlsb33','x11b33','neg33','not33']:
    for a in V:
        li('$r1', a); li('$r3', 0x12121212); e('%s $r3,$r1' % op); save('$r3')
for op in ['mul33','xor33','and33','or33']:
    for a in VS:
        for b in VS[:6]:
            li('$r3', a); li('$r1', b); e('%s $r3,$r1' % op); save('$r3')
for op in ['bmski33','fexti33']:
    for a in VS:
        for i in range(8):
            li('$r3', a); e('%s $r3,%d' % (op, i)); save('$r3')
for a in VS:
    li('$r1', a); e('mov55 $r2,$r1'); save('$r2')
for i in (-16, -1, 0, 15):
    e('movi55 $r2,%d' % i); save('$r2')
for i in (16, 17, 31, 47):
    e('movpi45 $r2,%d' % i); save('$r2')
li('$r4', 0x4444); li('$r5', 0x5555); e('movd44 $r6,$r4'); save('$r6', '$r7')
e('sethi $r12, 0x500'); e('addi $sp,$r12,100')
for i in (0, 4, 252): e('addri36.sp $r2,%d' % i); save('$r2')
for i in (-512, -4, 4, 508): e('addi10.sp %d' % i); save('$sp')
e('sethi $sp, 0x600')
group('ifc')
e('movi $r3,0'); e('movi $r4,0'); e('movi $r5,0'); e('movi $lp,0')
e('j IFCA')
e('.org 0x100')
e('IFCA:')
e('ifcall9 0xf0')           # 0x100 -> 0x1f0
e('addi333 $r3,$r3,1')      # return point 0x102
e('mfsr $r6,$psw'); e('bitci $r6,$r6,0x18'); e('j IFCB')
e('.org 0x1f0')
e('addi $r4,$r4,10')        # 0x1f0 (4 bytes)
e('mfsr $r7,$psw'); e('bitci $r7,$r7,0x18')
e('ifcall9 0x46')           # nested, 0x1fa -> 0x240
e('.org 0x240')
e('addi $r4,$r4,1000')
e('mfsr $r8,$psw'); e('bitci $r8,$r8,0x18')
e('ifret16')
e('.org 0x300')
e('IFCB:')
save('$r3', '$r4', '$r6', '$r7', '$r8')
e('mfusr $r5,$ifc_lp'); save('$r5')
e('mfsr $r6,$psw'); e('bitci $r6,$r6,0x18'); save('$r6')
e('movi $r3,0')
e('j IFCC')
e('.org 0x400')
e('IFCC:')
e('ifcall 0x100')           # 32-bit form, forward 0x100 -> 0x500
e('addi333 $r3,$r3,1')      # return point
e('j IFCD')
e('.org 0x500')
e('addi $r3,$r3,10')
e('.byte 0x4a,0x00,0x00,0x60')   # 32-bit ifret
e('.org 0x600')
e('IFCD:')
save('$r3')
# ifret without a pending ifcall does not branch
e('ifret16'); e('addi $r3,$r3,100'); save('$r3')
e('.byte 0x4a,0x00,0x00,0x60'); e('addi $r3,$r3,1000'); save('$r3')
e('mfsr $r6,$psw'); e('bitci $r6,$r6,0x18'); save('$r6')

group('bytes')
BV = [0x11223344, 0x11003344, 0x00223344, 0x11223300, 0x44444444, 0x11441144, 0x00000000, 0xffffffff, 0x11223355, 0x55223344, 0x12345678]
for op in ['ffb','ffmism','flmism','ffzmism','pbsad','pbsada']:
    binop('%s $r3,$r1,$r2' % op, vals=BV)
for a in BV:
    for imm in (0x11, 0x22, 0x33, 0x44, 0x00, 0xff, 0x77):
        li('$r1', a); li('$r3', 0x5555); CUR[0] = 'ffbi a=%#x b=%#x' % (a, imm); e('ffbi $r3,$r1,%d' % imm); save('$r3')

group('corner')
e('sethi $r12, 0x500'); e('sethi $r20, 0x500')
for i in range(16):
    li('$r1', 0x10203040 + i * 0x01010101); e('swi $r1,[$r12+%d]' % (i * 4))
CUR[0] = 'corner'
for op, post in [('lbi.bi',1),('lhi.bi',2),('lwi.bi',4),('lbsi.bi',1),('lhsi.bi',2)]:
    e('addi $r1,$r12,16'); e('%s $r1,[$r1],%d' % (op, post)); save('$r1')
for op in ['lb.bi','lh.bi','lw.bi','lbs.bi','lhs.bi']:
    e('addi $r1,$r12,16'); li('$r2', 4); e('%s $r1,[$r1],$r2' % op); save('$r1')
    e('addi $r2,$r12,16'); e('%s $r1,[$r2],$r2' % op); save('$r1', '$r2')
for op in ['sbi.bi','shi.bi','swi.bi']:
    e('addi $r1,$r12,16'); e('%s $r1,[$r1],4' % op); save('$r1')
    for i in range(0, 8): e('lwi $r3,[$r12+%d]' % (i * 4 + 12)); save('$r3')
for rb, re, en, form in [(1,3,0,'bim'),(1,3,0,'adm'),(2,4,0,'bi'),(1,1,0,'aim')]:
    for i in range(0, 8): li('$r%d' % (i + 1), 0x3000 + i); 
    e('addi $r1,$r12,16'); e('lmw.%s $r%d,[$r1],$r%d,%d' % (form, rb, re, en))
    save('$r1', '$r2', '$r3', '$r4')
for op in ['divr','divsr']:
    li('$r1', 100); li('$r2', 7); li('$r3', 0x1234)
    e('%s $r3,$r3,$r1,$r2' % op); save('$r3')
    li('$r1', 100); li('$r2', 7)
    e('%s $r1,$r2,$r1,$r2' % op); save('$r1', '$r2')
    li('$r1', 100); li('$r2', 7)
    e('%s $r2,$r1,$r1,$r2' % op); save('$r1', '$r2')
for op in ['mulr64','mulsr64']:
    li('$r1', 0x10001); li('$r2', 0x20003)
    e('%s $r2,$r1,$r2' % op); save('$r1', '$r2', '$r3')
    li('$r4', 0x10001); li('$r5', 0x20003)
    e('%s $r4,$r4,$r5' % op); save('$r4', '$r5')
li('$r1', 0x12345678)
e('cmovz $r1,$r1,$r1'); save('$r1')
e('cmovn $r1,$r1,$r1'); save('$r1')
li('$r1', 0x11); li('$r2', 0x22)
e('movd44 $r2,$r0'); li('$r0', 0xaa); li('$r1', 0xbb); li('$r2', 0xcc); li('$r3', 0xdd)
e('movd44 $r2,$r0'); save('$r0', '$r1', '$r2', '$r3')
e('movd44 $r0,$r2'); save('$r0', '$r1', '$r2', '$r3')
li('$r5', 0x4000)
e('la $r6,CORN1'); e('jral $r5,$r6'); e('CORN1:'); save('$r5')
e('la $r6,CORN2'); e('jral $r6,$r6'); e('CORN2:'); save('$r6')
e('la $r6,CORN3'); e('jralnez $r6,$r6'); e('CORN3:'); save('$r6')
li('$r1', 1); e('addi.gp $r2,0'); e('mov55 $r3,$sp'); 


def checksum(words):
    c = 0
    for w in words:
        c = ((c >> 5) | (c << 27)) & 0xffffffff
        c = ((c ^ w) + w) & 0xffffffff
    return c

FINISH = """\tsethi $r15, 0xf000
\tmovi $r14, 0x5555
\tswi $r14,[$r15+0]
\tj done
"""

def emit(names, expect):
    GC[_cur[0]] = CNT[0] - _cur[1]
    s = hdr
    for n in names:
        s += '\n'.join(body_groups[n]) + '\n'
    if expect is not None:
        s += """\tsethi $r1, 0x400
\tmovi $r3, 0
1:\tlwi.bi $r4,[$r1],4
\trotri $r3,$r3,5
\txor $r3,$r3,$r4
\tadd $r3,$r3,$r4
\tbne $r1,$r10,1b
\tsethi $r5,%d
\tori $r5,$r5,%d
\tbne $r3,$r5,2f
""" % (expect >> 12, expect & 0xfff)
        s += FINISH
        s += """2:\tsethi $r15, 0xf000
\tsethi $r14, 0x10
\tori $r14, $r14, 0x3333
\tswi $r14,[$r15+0]
"""
    s += 'done:\n\tj done\n'
    return s

if __name__ == '__main__':
    GC[_cur[0]] = CNT[0] - _cur[1]
    args = sys.argv[1:]
    if args[0] == '--list':
        print(' '.join(body_groups))
        sys.exit(0)
    expect = None
    names = []
    i = 0
    while i < len(args):
        if args[i] == '--expect':
            expect = int(args[i + 1], 16)
            i += 2
        elif args[i] == '--count':
            print(sum(GC[n] for n in args[i + 1:]))
            sys.exit(0)
        else:
            names.append(args[i])
            i += 1
    sys.stdout.write(emit(names, expect))
