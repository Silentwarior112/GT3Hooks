"""Compare an injected output against its base: are all original loadable
segments byte-identical (modulo the patched hook word), is the plugin segment
present and does it match the plugin ELF's .text?"""
import sys, struct

def phdrs(d):
    e_phoff, = struct.unpack_from('<I', d, 28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', d, 42)
    out=[]
    for i in range(e_phnum):
        out.append(struct.unpack_from('<IIIIIIII', d, e_phoff+i*e_phentsize))
    return out

def shdrs(d):
    e_shoff, = struct.unpack_from('<I', d, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 46)
    out=[]
    n=len(d)
    for i in range(e_shnum):
        o=e_shoff+i*e_shentsize
        if o+40>n: break
        out.append(struct.unpack_from('<IIIIIIIIII', d, o))
    names=b''
    if e_shstrndx < len(out):
        f=out[e_shstrndx]; names=d[f[4]:f[4]+f[5]]
    def nm(x):
        e=names.find(b'\0',x); return names[x:e].decode('latin1') if x<len(names) else f'<{x}>'
    return [(nm(s[0]),)+s for s in out]

base=open(sys.argv[1],'rb').read()
out =open(sys.argv[2],'rb').read()
plug=open(sys.argv[3],'rb').read() if len(sys.argv)>3 else None

print(f"base {len(base)} -> out {len(out)}  (+{len(out)-len(base)})")
bp=phdrs(base); op=phdrs(out)
print(f"phnum {len(bp)} -> {len(op)}")

# match original segments by (type, vaddr)
for i,(t,off,va,pa,fsz,msz,fl,al) in enumerate(bp):
    if t==0: print(f"  base seg[{i}] PT_NULL (empty) - dropped/kept?"); continue
    hit=None
    for j,(t2,off2,va2,pa2,fsz2,msz2,fl2,al2) in enumerate(op):
        if t2==t and va2==va: hit=(j,off2,fsz2,msz2,fl2,al2); break
    if hit is None:
        print(f"  *** base seg[{i}] type={t} va=0x{va:X} MISSING IN OUTPUT ***"); continue
    j,off2,fsz2,msz2,fl2,al2=hit
    same_len = (fsz==fsz2 and msz==msz2 and fl==fl2 and al==al2)
    bb=base[off:off+fsz]; ob=out[off2:off2+fsz2]
    if bb==ob:
        cmp="IDENTICAL"
    else:
        diffs=[k for k in range(min(len(bb),len(ob))) if bb[k]!=ob[k]]
        cmp=f"{len(diffs)} byte diffs; first at seg+0x{diffs[0]:X} => va 0x{va+diffs[0]:08X}" if diffs else f"len differs {len(bb)} vs {len(ob)}"
    print(f"  seg[{i}]->out[{j}] type={t} va=0x{va:08X} off 0x{off:X}->0x{off2:X} "
          f"filesz 0x{fsz:X}->0x{fsz2:X} memsz 0x{msz:X}->0x{msz2:X} fl {fl}->{fl2} al 0x{al:X}->0x{al2:X} "
          f"{'hdrs-same' if same_len else 'HDRS CHANGED'} | data {cmp}")
    # p_offset % p_align must equal p_vaddr % p_align
    if al>1 and (off2 % al) != (va2 % al):
        print(f"    *** ALIGNMENT VIOLATION: off%align=0x{off2%al:X} vaddr%align=0x{va2%al:X}")

# new segments
for j,(t2,off2,va2,pa2,fsz2,msz2,fl2,al2) in enumerate(op):
    if not any(t==t2 and va==va2 for (t,off,va,pa,fsz,msz,fl,al) in bp):
        print(f"  NEW out seg[{j}] type={t2} off=0x{off2:X} va=0x{va2:08X} filesz=0x{fsz2:X} memsz=0x{msz2:X} fl={fl2} al=0x{al2:X}")
        if off2+fsz2 > len(out): print("    *** runs past EOF ***")

# section diff
bs={s[0] for s in shdrs(base)}
os_=shdrs(out)
print("sections: base", len(shdrs(base)), "-> out", len(os_))
for s in os_:
    if s[0] not in bs:
        print(f"  NEW section {s[0]!r} type={s[2]} addr=0x{s[4]:08X} off=0x{s[5]:X} size=0x{s[6]:X}")
lost=bs-{s[0] for s in os_}
if lost: print("  LOST sections:", sorted(lost))

# plugin .text check
if plug:
    ps=shdrs(plug)
    pt=[s for s in ps if s[0]=='.text'][0]
    ptxt=plug[pt[5]:pt[5]+pt[6]]
    print(f"plugin .text addr=0x{pt[4]:08X} size=0x{pt[6]:X}")
    pl=[s for s in os_ if s[0]=='.plugin']
    if not pl: print("  *** no .plugin section in output ***")
    else:
        s=pl[0]; got=out[s[5]:s[5]+s[6]]
        print(f"  .plugin addr=0x{s[4]:08X} size=0x{s[6]:X} content {'MATCHES plugin .text' if got==ptxt else 'DIFFERS'}")
    # INVOKER
    for s in ps:
        if s[2]==2:  # SYMTAB
            strt=ps[s[7]]
            sd=plug[s[5]:s[5]+s[6]]; st=plug[strt[5]:strt[5]+strt[6]]
            for k in range(0,len(sd),16):
                nmo,val,sz,info,other,shndx=struct.unpack_from('<IIIBBH',sd,k)
                e=st.find(b'\0',nmo)
                if st[nmo:e]==b'INVOKER':
                    print(f"  INVOKER = 0x{val:08X}")
                    exp=0x0C000000 | ((val & 0x0FFFFFFF)>>2)
                    # locate hook word in output
                    for (t2,off2,va2,pa2,fsz2,msz2,fl2,al2) in op:
                        if t2==1 and va2<=0x100094<va2+fsz2:
                            fo=off2+(0x100094-va2)
                            w=struct.unpack_from('<I',out,fo)[0]
                            print(f"  word@0x00100094 = 0x{w:08X}, expected jal = 0x{exp:08X}  {'OK' if w==exp else 'MISMATCH'}")
