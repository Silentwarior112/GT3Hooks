import sys, struct, os

SHT = {0:'NULL',1:'PROGBITS',2:'SYMTAB',3:'STRTAB',4:'RELA',5:'HASH',6:'DYNAMIC',7:'NOTE',
       8:'NOBITS',9:'REL',10:'SHLIB',11:'DYNSYM',0x70000006:'MIPS_REGINFO',
       0x7000000d:'MIPS_OPTIONS',0x70000000:'MIPS_LIBLIST'}
PT = {0:'NULL',1:'LOAD',2:'DYNAMIC',3:'INTERP',4:'NOTE',5:'SHLIB',6:'PHDR',
      0x70000000:'MIPS_REGINFO'}

def report(path):
    d = open(path,'rb').read()
    n = len(d)
    print("="*78)
    print(f"{os.path.basename(path)}  size=0x{n:X} ({n})")
    assert d[:4]==b'\x7fELF', "not elf"
    (ei_class, ei_data, ei_ver) = d[4],d[5],d[6]
    (e_type,e_machine,e_version,e_entry,e_phoff,e_shoff,e_flags,
     e_ehsize,e_phentsize,e_phnum,e_shentsize,e_shnum,e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
    print(f" class={ei_class} data={ei_data} type={e_type} machine={e_machine} entry=0x{e_entry:08X} flags=0x{e_flags:08X}")
    print(f" e_phoff=0x{e_phoff:X} phentsize={e_phentsize} phnum={e_phnum}")
    print(f" e_shoff=0x{e_shoff:X} shentsize={e_shentsize} shnum={e_shnum} shstrndx={e_shstrndx}")
    sh_end = e_shoff + e_shnum*e_shentsize
    print(f" section table span 0x{e_shoff:X}..0x{sh_end:X}  EOF=0x{n:X}  "
          f"{'TRUNCATED, missing %d bytes (%d of %d headers present)'%(sh_end-n,(n-e_shoff)//e_shentsize,e_shnum) if sh_end>n else 'OK'}")
    print(" PROGRAM HEADERS:")
    for i in range(e_phnum):
        off = e_phoff+i*e_phentsize
        if off+32 > n: print(f"  [{i}] PAST EOF"); continue
        p_type,p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_flags,p_align = struct.unpack_from('<IIIIIIII', d, off)
        tail = ""
        if p_offset+p_filesz > n: tail = "  *** filesz runs past EOF ***"
        print(f"  [{i}] {PT.get(p_type,hex(p_type)):<14} off=0x{p_offset:06X} va=0x{p_vaddr:08X} pa=0x{p_paddr:08X} "
              f"filesz=0x{p_filesz:X} memsz=0x{p_memsz:X} fl={p_flags} align=0x{p_align:X}{tail}")
    # section headers
    print(" SECTION HEADERS:")
    navail = max(0, (n - e_shoff)//e_shentsize) if e_shoff < n else 0
    shown = min(e_shnum, navail)
    # get shstrtab
    names = b''
    if shown > e_shstrndx:
        so = e_shoff + e_shstrndx*e_shentsize
        f = struct.unpack_from('<IIIIIIIIII', d, so)
        names = d[f[4]:f[4]+f[5]]
    def nm(x):
        if x >= len(names): return f"<off {x}>"
        e = names.find(b'\0', x); return names[x:e].decode('latin1')
    print(f"   headers present in file: {navail} of {e_shnum}")
    for i in range(min(shown, 40)):
        so = e_shoff + i*e_shentsize
        (s_name,s_type,s_flags,s_addr,s_off,s_size,s_link,s_info,s_align,s_entsize)=struct.unpack_from('<IIIIIIIIII',d,so)
        print(f"  [{i:2}] {nm(s_name):<28} {SHT.get(s_type,hex(s_type)):<12} addr=0x{s_addr:08X} off=0x{s_off:06X} size=0x{s_size:X} al={s_align}")
    if shown > 40:
        print(f"   ... {shown-40} more; last few:")
        for i in range(max(40,shown-4), shown):
            so = e_shoff + i*e_shentsize
            (s_name,s_type,s_flags,s_addr,s_off,s_size,s_link,s_info,s_align,s_entsize)=struct.unpack_from('<IIIIIIIIII',d,so)
            print(f"  [{i:4}] {nm(s_name):<40} {SHT.get(s_type,hex(s_type)):<10} addr=0x{s_addr:08X} off=0x{s_off:06X} size=0x{s_size:X}")
    # word at 0x100094
    for i in range(e_phnum):
        off = e_phoff+i*e_phentsize
        if off+32>n: continue
        p_type,p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_flags,p_align = struct.unpack_from('<IIIIIIII', d, off)
        if p_type==1 and p_vaddr <= 0x100094 < p_vaddr+p_filesz:
            fo = p_offset + (0x100094-p_vaddr)
            w = struct.unpack_from('<I', d, fo)[0]
            w2 = struct.unpack_from('<I', d, fo+4)[0]
            print(f" word @va 0x00100094 (file 0x{fo:X}) = 0x{w:08X}", end='')
            if w == 0x42000038: print("  (ei, UNPATCHED)")
            elif (w>>26)==3: print(f"  (JAL -> 0x{((0x100094>>28)<<28)|((w&0x03FFFFFF)<<2):08X})")
            else: print()
            print(f" word @va 0x00100098 (delay slot) = 0x{w2:08X}")
    # find first raw occurrence of 38 00 00 42 in file
    p = d.find(b'\x38\x00\x00\x42')
    print(f" first raw '38 00 00 42' in file at offset 0x{p:X} ({p}){'  [ALIGNED]' if p%4==0 else '  [UNALIGNED!]'}")

for a in sys.argv[1:]:
    report(a)
