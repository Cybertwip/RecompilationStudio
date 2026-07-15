#!/usr/bin/env python3
"""Regression: a function returning through a register that saved $ra stays code."""
from __future__ import annotations
import json, struct, subprocess, sys, tempfile
from pathlib import Path

def jal(pc:int,target:int)->int:return 0x0C000000|((target>>2)&0x03FFFFFF)

def main()->int:
    if len(sys.argv)!=2: raise SystemExit('usage: test_saved_ra_return_extent.py <psxrecomp-game>')
    tool=Path(sys.argv[1]).resolve(); load=0x80100000; entry=load; fn=load+0x40
    code=bytearray(0x200)
    words={
      0x00:jal(entry,fn),0x04:0,0x08:0x03E00008,0x0C:0,
      0x40:0x03E04021, # move t0,ra
      0x44:0x0C000000|(((load+0x100)>>2)&0x03FFFFFF),0x48:0,
      0x4C:0x01000008, # jr t0
      0x50:0x00E01021, # move v0,a3
      0x54:0xFFFFFFFF,0x58:0x11111111,
      0x100:0x03E00008,0x104:0,
    }
    for o,w in words.items():struct.pack_into('<I',code,o,w)
    h=bytearray(0x800);h[:8]=b'PS-X EXE';struct.pack_into('<I',h,0x10,entry);struct.pack_into('<I',h,0x18,load);struct.pack_into('<I',h,0x1c,len(code));struct.pack_into('<I',h,0x30,0x801ffff0)
    with tempfile.TemporaryDirectory(prefix='psx-saved-ra-') as td:
      r=Path(td);(r/'.gitignore').write_text('*\n');(r/'game.exe').write_bytes(h+code);(r/'seeds.txt').write_text(f'0x{entry:08X}\n')
      (r/'game.toml').write_text(f'''[game]\nname="saved-ra"\nid="TEST-00002"\nexe="game.exe"\nload_address="0x{load:08X}"\nentry_pc="0x{entry:08X}"\ntext_size="0x{len(code):X}"\nstack_base="0x801FFFF0"\n[recompiler]\nseeds="seeds.txt"\nout_dir="generated"\nstrict=true\n[audit]\n[[audit.regions]]\nname="Text"\nrom_start="0x800"\nrom_end="0x{0x800+len(code):X}"\nvaddr_base="0x{load:08X}"\n[audit.normalize]\nkseg_mask="0x1FFFFFFF"\n''')
      p=subprocess.run([str(tool),'--config',str(r/'game.toml')],cwd=r,text=True,capture_output=True)
      if p.returncode:print(p.stdout);print(p.stderr,file=sys.stderr);return 1
      ranges=(r/'generated/game.exe_full.ranges').read_text()
      if f'F {fn:08X}' not in ranges or f'R {fn:08X} 14' not in ranges:raise SystemExit('saved-ra function was not bounded to 20 bytes')
      data=json.loads((r/'generated/game.exe_dynamic_targets.json').read_text())
      if any(int(x['target'],16)==fn for x in data['targets']):raise SystemExit('saved-ra function misclassified as runtime-installed data')
    print('saved-ra return extent regression passed');return 0
if __name__=='__main__':raise SystemExit(main())
