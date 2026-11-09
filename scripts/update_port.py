"""Game-update port helper: every PBZ.exe address the DLL source pins, found again in a new PBZ.exe.

  update_port.py snapshot   BEFORE an update (needs the current, accepted PBZ.exe): exe facts + a masked byte
                            signature for every pinned address -> local/data/exe_signatures.json
  update_port.py locate     AFTER the update: every signature searched in the new PBZ.exe -> local/data/exe_relocated.json
  update_port.py apply      rewrites the pinned constants in src/ and tests/ (proxy exe pins, RVAs, the code-section
                            bound) from exe_relocated.json, and re-reads the code bytes the DLL compares at them;
                            refuses while any address is missing or ambiguous
  update_port.py audit      any time: every pin has a signature, every code-byte self-check matches the exe (or, for
                            checks against code found at runtime, still occurs in it), no unclassified address literals;
                            with --exe set to another accepted build (WeGame) it checks that build the same way
  update_port.py build wegame   the WeGame PBZ.exe of the same changelist (after apply + snapshot on Steam): every pin
                            located again and required at the same RVA (no per-build pin sets: it refuses otherwise),
                            the self-checks whose bytes differ only by a jump into the protector section get WeGame
                            rows in src/core/game_build.cpp, proxy.cpp gets its kWeGame* exe facts;
                            -> local/data/exe_relocated_wegame.json

Pinned addresses are the hex literals in src/ and tests/ on lines that add them to ue::Base() or name an RVA (k*Rva,
ue::layout), in a table marked `// exe-pins` (to the line ending `};`), or on a line tagged `// exe-bytes: <address>`.
Code-byte self-checks are `unsigned char` arrays and inline byte lists: one compared at a pinned address (a line that
names the array after the address, a table row with the list after the address, or the exe-bytes tag) is re-read by
apply; the rest compare code found at runtime and audit only says whether the bytes still occur in the exe.
Code addresses (in the code section .xcode) get a signature from their own first instructions, RIP-
relative displacements and branch targets masked; a signature grows until it is unique. Data addresses (globals,
vtables) get signatures of up to three instructions that reference them RIP-relative; locate decodes the matched
instruction and takes the address it points at (the sites must agree). Engine-level layout (UObject / FProperty
offsets, vtable slots) is not here: ue::ValidateLayout checks it at runtime and only an engine upgrade moves it.
"""
import argparse, hashlib, json, re, sys
from itertools import groupby
from pathlib import Path
import capstone, numpy as np, pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT/'src'
DATA = ROOT/'local'/'data'
SIGS, MOVED = DATA/'exe_signatures.json', DATA/'exe_relocated.json'
CODE = '.xcode'  # the 11-04 exe's name for it; the protector renames sections per build (11-05: .idata), so code() goes by flags
# Not addresses: coarse bounds. 0xa600000 = start of the last MiB of .xcode (vtable entries are code below it).
BOUNDS = {0xa600000: 'code-end-mib', 0x20000000: None}
LITERAL = re.compile(r'0x[0-9a-fA-F]{5,9}\b')
CONTEXT = re.compile(r'Base\(\)|Rva\b|Rva\s*=|constexpr uintptr_t k\w+\s*=|\{0x[0-9a-fA-F]+,\{0x')  # last: {rva,{16 code bytes}} self-checks
TESTS = ROOT/'tests'
PIN_BLOCK = re.compile(r'//.*\bexe-pins\b')
BYTES_TAG = re.compile(r'//.*\bexe-bytes:\s*(\w+)')
ARRAY = re.compile(r'unsigned char (\w+)\[\w*\]\s*=\s*\{')
BYTE = re.compile(r'0x[0-9a-fA-F]{1,2}\b|\b\d{1,3}\b')
INLINE = re.compile(r'\{\s*(?:(?:0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*){3,}(?:0x[0-9a-fA-F]{1,2}|\d{1,3})\s*\}')
ADDRESS = re.compile(r'0x[0-9a-fA-F]{5,9}\b|\bk\w+')
# Literals that are not addresses: property kinds / flags, sizes, an IOCTL code.
NOT_ADDRESSES = {0x10000, 0x20000, 0x40000, 0x100000, 0x200000, 0x400000, 0x8000000, 0x10000000, 0xb0195,
                 0xffffff}  # Steam CGameID AppID mask, not an RVA.
# Other shipping builds of the same changelist: the proxy.cpp constants k<prefix>Timestamp ..., the GameBuild::Id of
# their game_build.cpp rows (the primary build is Steam, with plain kTimestamp ...).
BUILDS = {'wegame': dict(prefix='WeGame', id='WeGame')}
PROXY_PINS = re.compile(r'\bk(?:' + '|'.join(b['prefix'] for b in BUILDS.values()) + r')?(Timestamp|ImageSize|Entrypoint|ExeBytes|Sha256)\b')
GAME_BUILD = SRC/'core'/'game_build.cpp'
BUILD_BYTES = re.compile(r'//.*\bexe-build-bytes\b')  # marks the per-build byte table (also an exe-pins block)
BUILD_ROW = re.compile(r'^\s*\{Id::(\w+),\s*(0x[0-9a-fA-F]+),\s*(\d+),\s*(\d+),\s*\{([^}]*)\},\s*\{([^}]*)\}\},')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True


class Image:
    def __init__(self, path):
        self.path = Path(path); self.raw = self.path.read_bytes(); pe = pefile.PE(data=self.raw, fast_load=True)
        self.timestamp, self.size, self.entry = pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage, pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.sections = [(s.Name.rstrip(b'\0').decode('latin1'), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Characteristics)
                         for s in pe.sections]
        self.sha256 = hashlib.sha256(self.raw).hexdigest()

    def section(self, rva):
        return next((s for s in self.sections if s[1] <= rva < s[1] + max(s[2], s[4])), None)

    def bytes_at(self, rva, n):
        s = self.section(rva); off = s[3] + rva - s[1]; return self.raw[off:off + n]

    def code(self):
        # The executable, non-writable code section (0x60000020); the writable executable one (0xe0000020) is the protector's.
        s = next(s for s in self.sections if s[5] & 0x20000020 == 0x20000020 and not s[5] & 0x80000000); return s, self.raw[s[3]:s[3] + s[4]]


def sources():
    return [p for d in (SRC, TESTS) for p in sorted(d.rglob('*')) if p.suffix in ('.cpp', '.h') and 'generated' not in p.parts]


def pin_lines(lines):
    """Line numbers (1-based) whose hex literals are exe addresses."""
    out = set(); in_layout = in_block = False
    for n, line in enumerate(lines, 1):
        if 'namespace layout' in line: in_layout = True
        if in_layout and line.startswith('}'): in_layout = False
        if PIN_BLOCK.search(line): in_block = True
        if (CONTEXT.search(line) or in_layout or in_block or BYTES_TAG.search(line)) and not PROXY_PINS.search(line): out.add(n)
        if in_block and line.split('//')[0].rstrip().endswith('};'): in_block = False
    return out


def build_byte_lines(lines):
    """Line numbers (1-based) of the per-build byte table (game_build.cpp): its pins count, its byte lists are not
    self-checks."""
    out = set(); inside = False
    for n, line in enumerate(lines, 1):
        if BUILD_BYTES.search(line): inside = True
        if inside: out.add(n)
        if inside and line.split('//')[0].rstrip().endswith('};'): inside = False
    return out


def build_rows():
    """The per-build byte rows: [dict(build, pin, offset, steam=bytes, other=bytes)]."""
    out = []
    for line in GAME_BUILD.read_text(encoding='utf-8').splitlines():
        m = BUILD_ROW.match(line)
        if not m: continue
        count = int(m.group(4))
        steam = bytes(int(x, 0) for x in m.group(5).split(',') if x.strip()); other = bytes(int(x, 0) for x in m.group(6).split(',') if x.strip())
        assert len(steam) == len(other) == count, line
        out.append(dict(build=m.group(1), pin=int(m.group(2), 16), offset=int(m.group(3)), steam=steam, other=other))
    return out


def with_rows(data, address, build_id, rows):
    """The bytes a self-check at address expects on build_id: CodeMatches' substitution (None where a row's Steam bytes
    differ from the source's: a stale row, the check fails in game)."""
    want = bytearray(data)
    for r in rows:
        if r['build'] != build_id: continue
        for i in range(len(r['steam'])):
            at = r['pin'] + r['offset'] + i
            if not address <= at < address + len(want): continue
            if want[at - address] != r['steam'][i]: return None
            want[at - address] = r['other'][i]
    return bytes(want)


def proxy_facts(prefix=''):
    """The exe facts proxy.cpp accepts for a build (prefix '' = the primary build, Steam)."""
    text = (SRC/'core'/'proxy.cpp').read_text(encoding='utf-8'); out = {}
    for name, key in (('Timestamp', 'timestamp'), ('ImageSize', 'image_size'), ('Entrypoint', 'entry'), ('ExeBytes', 'bytes'), ('Sha256', 'sha256')):
        m = re.search(rf'\bk{prefix}{name}\s*=\s*"?([0-9a-fA-Fx]+)"?;', text)
        out[key] = None if not m else (m.group(1) if name == 'Sha256' else int(m.group(1), 0))
    return out


def pinned():
    """{value: [(file, line, literal)]} of the pinned addresses in src/ and tests/."""
    out = {}
    for path in sources():
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
        for n in sorted(pin_lines(lines)):
            for m in LITERAL.finditer(lines[n - 1].split('//')[0]):  # old values quoted in comments are not pins
                v = int(m.group(0), 16)
                if v < 0x10000 or v in NOT_ADDRESSES: continue
                out.setdefault(v, []).append((str(path.relative_to(ROOT)), n, m.group(0)))
    return out


def constants(text):
    """The k-constants a file defines from hex literals: {name: value}."""
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(r'\b(k\w+)\s*=\s*(0x[0-9a-fA-F]+)\b', text)}


def address_before(line, end, consts, pins):
    """The pinned address a line names last before column `end`: a hex literal or a k-constant."""
    best = None
    for m in ADDRESS.finditer(line[:end]):
        t = m.group(0); v = int(t, 16) if t.startswith('0x') else consts.get(t)
        if v in pins: best = v
    return best


def code_arrays(pins=None):
    """Byte lists the DLL compares with exe code: [dict(file, line, name, tokens=[(start, end, text, value)], address)].
    address is the pinned address the list is compared at, None when the DLL compares it with code found at runtime."""
    pins = pins if pins is not None else pinned(); out = []
    for path in sources():
        # Raw text (CRLF kept): apply edits the files at these offsets.
        text = path.read_bytes().decode('utf-8', 'replace'); consts = constants(text); lines = text.split('\n')
        offsets = [0]
        for line in lines: offsets.append(offsets[-1] + len(line) + 1)
        def tokens(start, end):
            return [(start + t.start(), start + t.end(), t.group(0), int(t.group(0), 0)) for t in BYTE.finditer(text[start:end])]
        for m in ARRAY.finditer(text):
            name, start = m.group(1), m.end(); end = text.index('}', start); n = text.count('\n', 0, m.start()) + 1
            tag = BYTES_TAG.search(lines[n - 1]); address = None
            if tag:
                t = tag.group(1); address = int(t, 16) if t.startswith('0x') else consts.get(t)
            else:
                # Uses in the array's scope: the rest of its function (indented declaration) or the whole file.
                last = next((i for i in range(n, len(lines)) if lines[i].startswith('}')), len(lines)) if lines[n - 1][:1].isspace() else len(lines)
                for i in range(n, last + 1):
                    if i == n: continue
                    for u in re.finditer(r'\b' + re.escape(name) + r'\b', lines[i - 1]):
                        a = address_before(lines[i - 1], u.start(), consts, pins)
                        if a is not None: address = a
                    if address is not None: break
            out.append(dict(file=str(path.relative_to(ROOT)), line=n, name=name, tokens=tokens(start, end), address=address))
        table = build_byte_lines(lines)
        for n, line in enumerate(lines, 1):
            if ARRAY.search(line) or n in table: continue
            for lm in INLINE.finditer(line):
                a = address_before(line, lm.start(), consts, pins)
                if a is None: continue
                base = offsets[n - 1]
                out.append(dict(file=str(path.relative_to(ROOT)), line=n, name=None, tokens=tokens(base + lm.start(), base + lm.end()), address=a))
    return out


def variable(img, rva, n):
    """Per byte from rva: True where an update may change it without the code changing (RIP displacements, branch
    targets, and everything after a ret / unconditional jmp, which is the next function's or padding)."""
    data = img.bytes_at(rva, n + 16); out = []
    for insn in md.disasm(data, rva):
        flags = [False] * insn.size
        if insn.disp_size and any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in insn.operands):
            for i in range(insn.disp_offset, insn.disp_offset + insn.disp_size): flags[i] = True
        if insn.imm_size and (insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL)):
            for i in range(insn.imm_offset, insn.imm_offset + insn.imm_size): flags[i] = True
        out += flags
        if insn.mnemonic in ('ret', 'jmp') or len(out) >= n: break
    return (out + [True] * n)[:n]


def masked(img, rva, length):
    """Bytes from rva with RIP-relative displacements and relative branch targets as None, whole instructions. Ends at
    a ret / jmp that closes the function (int3 padding follows) or is a thunk's only instruction: what follows is
    padding or another function (or the protector's junk) and differs between builds."""
    data = img.bytes_at(rva, length + 16); out = []
    for insn in md.disasm(data, rva):
        b = list(insn.bytes); first = not out
        if insn.disp_size and any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in insn.operands):
            for i in range(insn.disp_offset, insn.disp_offset + insn.disp_size): b[i] = None
        if insn.imm_size and (insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL)) and \
                any(op.type == capstone.x86.X86_OP_IMM for op in insn.operands):
            for i in range(insn.imm_offset, insn.imm_offset + insn.imm_size): b[i] = None
        out += b
        if insn.mnemonic in ('ret', 'jmp') and (first or data[len(out):len(out) + 1] == b'\xcc'): break
        if len(out) >= length: break
    return out


def regex(sig):
    return re.compile(b''.join(b'.' if x is None else re.escape(bytes([x])) for x in sig), re.S)


def hits(code, sig, limit=3):
    """Match positions, overlapping ones included (a signature inside repeated code matches at several close offsets)."""
    rx, out, pos = regex(sig), [], 0
    while len(out) < limit:
        m = rx.search(code, pos)
        if not m: break
        out.append(m.start()); pos = m.start() + 1
    return out


def text(sig):
    return ' '.join('??' if x is None else f'{x:02x}' for x in sig)


def parse(t):
    return [None if x == '??' else int(x, 16) for x in t.split()]


def unique_sig(img, code_sec, code, rva, lengths=(16, 24, 32, 48, 64, 96, 128)):
    for n in lengths:
        sig = masked(img, rva, n)
        if sum(x is not None for x in sig) < 8: continue
        if len(hits(code, sig, 2)) == 1: return sig
    # A whole leaf function shorter than 8 bytes (a flag setter: or byte [rcx+5a],4 / ret): its body and the int3 that
    # follows, when that is unique. Every byte is fixed; the DLL compares the same body before calling it.
    body = masked(img, rva, 16)
    if body and len(body) < 8 and body[-1] == 0xc3 and None not in body and img.bytes_at(rva + len(body), 1) == b'\xcc':
        sig = body + [0xcc]
        if len(sig) >= 5 and len(hits(code, sig, 2)) == 1: return sig
    return None


def xrefs(img, target, want=3):
    """Instructions in the code section that reference target RIP-relative: [(rva of the instruction, its length)]."""
    (name, va, vsize, off, rsize, _), code = img.code()
    arr = np.frombuffer(code, dtype=np.uint8); found = []
    for extra in (0, 1, 4):  # bytes after the displacement (an imm8 / imm32 operand)
        for chunk in range(0, len(arr) - 8, 1 << 24):
            a = arr[chunk:chunk + (1 << 24) + 8].astype(np.int64)
            if len(a) < 8: break
            disp = a[:-3] | (a[1:-2] << 8) | (a[2:-1] << 16) | (a[3:] << 24); disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
            pos = np.arange(len(disp), dtype=np.int64) + chunk
            for p in np.nonzero(va + pos + 4 + extra + disp == target)[0][:64]:
                p = int(p) + chunk
                for start in range(p - 1, p - 8, -1):
                    insn = next(md.disasm(code[start:start + 16], va + start, 1), None)
                    if not insn: continue
                    if insn.disp_offset == p - start and insn.address + insn.size + insn.disp == target and \
                            any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in insn.operands):
                        found.append((insn.address, insn.size)); break
                    if insn.imm_offset == p - start and (insn.group(capstone.CS_GRP_CALL) or insn.group(capstone.CS_GRP_JUMP)) and \
                            any(op.type == capstone.x86.X86_OP_IMM and op.imm == target for op in insn.operands):  # call / jmp rel32
                        found.append((insn.address, insn.size)); break
                if len(found) >= want * 4: break
    return sorted(set(found))


def pointers(img):
    """The image as aligned qwords, and its preferred base (absolute pointers in tables = base + rva)."""
    return np.frombuffer(img.raw[:len(img.raw) // 8 * 8], dtype=np.uint64), pefile.PE(data=img.raw[:0x1000], fast_load=True).OPTIONAL_HEADER.ImageBase


def native_name(img, qwords, base, index):
    """Name of a native function table entry {const char* name; FNativeFuncPtr func} whose func is qwords[index]."""
    p = int(qwords[index - 1])
    if not base <= p < base + img.size: return None
    try: name = img.bytes_at(p - base, 96).split(b'\0')[0].decode('ascii')
    except (UnicodeDecodeError, TypeError, AttributeError): return None
    return name if name and name.isprintable() else None


def native_candidates(img, entry):
    """Functions native tables register under entry['name'], the best neighbour match first, then in table order."""
    qwords, base = pointers(img); want = entry['name'].encode('ascii') + b'\0'; found = []
    for m in re.finditer(re.escape(want), img.raw):
        if m.start() and img.raw[m.start() - 1] != 0: continue
        off = m.start(); s = next((x for x in img.sections if x[3] <= off < x[3] + x[4]), None)
        if not s: continue
        for i in np.nonzero(qwords == base + s[1] + off - s[3])[0][:8]:
            i = int(i); func = int(qwords[i + 1]) - base
            if not 0 < func < img.size: continue
            score = (native_name(img, qwords, base, i - 1) == entry.get('before')) + (native_name(img, qwords, base, i + 3) == entry.get('after'))
            found.append((-score, i, func))
    found.sort(); best = [f for f in found if f[0] == found[0][0]] if found else []
    return [f[2] for f in best]


def native_entry(img, rva):
    """A code address registered in a native function table: its name, the names around it, and which of the equally
    good candidates it is (a base and a derived class can register identical tables)."""
    qwords, base = pointers(img)
    for i in np.nonzero(qwords == base + rva)[0][:4]:
        i = int(i); name = native_name(img, qwords, base, i)
        if not name: continue
        entry = dict(name=name, before=native_name(img, qwords, base, i - 2), after=native_name(img, qwords, base, i + 2))
        candidates = native_candidates(img, entry)
        if rva in candidates: entry.update(ordinal=candidates.index(rva), of=len(candidates)); return entry
    return None


def native_find(img, entry):
    """The function a native table names `entry['name']`; None unless the candidate count is the snapshot's."""
    candidates = native_candidates(img, entry)
    return candidates[entry.get('ordinal', 0)] if len(candidates) == entry.get('of', 1) else None


def found_ok(row):
    return locators(row) > 0 or row['kind'] in ('bound', 'ignore')


LOCATORS = ('signature', 'native', 'xrefs', 'vslot', 'xref_of', 'inside', 'near')


def locators(row):
    """How many independent ways the snapshot has to find this pin again. The neighbour distance is only a fallback
    (locate uses it when nothing else found the pin), so it counts only when it is the sole locator."""
    own = sum(bool(row.get(k)) for k in LOCATORS if k != 'near')
    return own or int(bool(row.get('near')))


def sites(img, s, code, value):
    """Signatures of up to three instructions that reference value (RIP-relative operand or call / jmp rel32)."""
    out = []
    for at, size in xrefs(img, value):
        for back in (0, 8, 16):
            sig = unique_sig(img, s, code, at - back) if back else unique_sig(img, s, code, at)
            if sig: out.append(dict(rva=at, offset=back, size=size, signature=text(sig))); break
        if len(out) >= 3: break
    return out


def rip_targets(img, lo, hi, limit=64):
    """RIP-relative memory operands in the code section whose target lies in [lo, hi): sorted {(target, at, size)}."""
    (name, va, vsize, off, rsize, _), code = img.code()
    arr = np.frombuffer(code, dtype=np.uint8); found = set()
    for extra in (0, 1, 4):
        for chunk in range(0, len(arr) - 8, 1 << 24):
            a = arr[chunk:chunk + (1 << 24) + 8].astype(np.int64)
            if len(a) < 8: break
            disp = a[:-3] | (a[1:-2] << 8) | (a[2:-1] << 16) | (a[3:] << 24); disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
            t = va + np.arange(len(disp), dtype=np.int64) + chunk + 4 + extra + disp
            for local in np.nonzero((t >= lo) & (t < hi))[0][:limit]:
                p = int(local) + chunk; target = int(t[local])
                for begin in range(p - 1, p - 8, -1):
                    insn = next(md.disasm(code[begin:begin + 16], va + begin, 1), None)
                    if insn and insn.disp_offset == p - begin and insn.address + insn.size + insn.disp == target and \
                            any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in insn.operands):
                        found.add((target, insn.address, insn.size)); break
    return sorted(found)


def vslot(img, s, code, value, tables):
    """A virtual function: the vtable slot that holds it, as table start + offset. The table is a pinned global the DLL
    reads (tables), or else the nearest address below the slot that code references (constructors load the table)."""
    q, base = pointers(img)
    for i in np.nonzero(q == base + value)[0][:4]:
        off = int(i) * 8; sec = next((x for x in img.sections if x[3] <= off < x[3] + x[4]), None)
        if not sec: continue
        slot = sec[1] + off - sec[3]
        table = max((t for t in tables if 0 <= slot - t < 0x800), default=None)
        if table is not None: return dict(table=table, delta=slot - table)
        refs = rip_targets(img, slot - 0x800, slot + 8)
        if not refs: continue
        table = max(r[0] for r in refs); table_sites = sites(img, s, code, table)
        if table_sites: return dict(table=table, delta=slot - table, sites=table_sites)
    return None


def snapshot(args):
    img = Image(args.exe); s, code = img.code(); rows = []
    old = json.loads(SIGS.read_text(encoding='utf-8')) if SIGS.exists() and not args.refresh else None
    reuse = {r['rva']: r for r in old['addresses'] if found_ok(r)} if old and old['sha256'] == img.sha256 else {}
    pins = sorted(pinned().items())
    for value, uses in pins:
        sec = img.section(value); row = dict(rva=value, uses=uses, section=sec[0] if sec else None)
        if value in reuse: row = dict(reuse[value], uses=uses)
        elif value in BOUNDS: row['kind'] = 'bound' if BOUNDS[value] else 'ignore'
        elif not sec: row['kind'] = 'unknown'
        else:
            # Independent locators where possible: the code's own bytes (or the native table that names it), the
            # instructions that call / reference it, the vtable slot that holds it; one changed function or call site
            # still leaves the others, and locate refuses when they disagree.
            if sec[0] == s[0]:
                sig = unique_sig(img, s, code, value); row.update(kind='code', signature=text(sig) if sig else None)
                if not sig:  # a body other functions share: a native table names it, or its call sites lead to it
                    row['native'] = native_entry(img, value); row['kind'] = 'native' if row['native'] else 'code-xref'
            else: row['kind'] = 'data'
            row['xrefs'] = sites(img, s, code, value)
        rows.append(row)
    tables = [r['rva'] for r in rows if r['kind'] == 'data']
    for row in rows:
        if row['rva'] in reuse or row['kind'] not in ('code', 'native', 'code-xref'): continue
        row['vslot'] = vslot(img, s, code, row['rva'], tables)
    # Globals: a fixed distance from the nearest other located global in the section, used only when nothing else finds
    # them (the only locator for a member of a static object that nothing references directly).
    for row in rows:
        if row['kind'] != 'data': continue
        near = min((r for r in rows if r is not row and r['kind'] == 'data' and r.get('xrefs') and r['section'] == row['section']),
                   key=lambda r: abs(r['rva'] - row['rva']), default=None)
        if near and abs(near['rva'] - row['rva']) < 0x10000: row['near'] = dict(rva=near['rva'], delta=row['rva'] - near['rva'])
        if not row.get('xrefs') and row.get('near'): row['kind'] = 'data-near'
    # An instruction in boilerplate (a console variable's registration: lea r8, storage) whose bytes are not unique even
    # masked: one of the RIP-relative references to a pinned global, picked by its masked bytes and order.
    data = {r['rva'] for r in rows if r['kind'] in ('data', 'data-near')}
    for row in rows:
        if row['kind'] != 'code-xref' or found_ok(row): continue
        insn = next(md.disasm(img.bytes_at(row['rva'], 16), row['rva'], 1), None)
        target = insn.address + insn.size + insn.disp if insn and insn.disp_size else None
        if target not in data: continue
        # xrefs() may start an instruction past its REX prefix; candidates are told apart by where they end.
        n = insn.size; pattern = text(masked(img, row['rva'], n)[:n])
        same = [end - n for end in sorted({at + size for at, size in xrefs(img, target)}) if text(masked(img, end - n, n)[:n]) == pattern]
        if row['rva'] in same: row.update(kind='xref-of', xref_of=dict(target=target, pattern=pattern, ordinal=same.index(row['rva']), of=len(same)))
    # An instruction inside a pinned function that nothing else finds (the call that marks an emptied exec): the same
    # distance from the function's start, checked against its masked bytes.
    starts = [r['rva'] for r in rows if r['kind'] in ('code', 'native') or r.get('vslot')]
    for row in rows:
        if row['kind'] not in ('code', 'code-xref') or found_ok(row): continue
        below = max((v for v in starts if 0 < row['rva'] - v < 0x400), default=None)
        if below is None: continue
        pattern = masked(img, row['rva'], 12)
        if sum(x is not None for x in pattern) >= 3: row.update(kind='inside', inside=dict(base=below, delta=row['rva'] - below, pattern=text(pattern)))
    for row in rows:
        print(f'{row["rva"]:#010x} {row["kind"]:9s} {row["section"] or "-":8s} uses={len(row["uses"]):2d} {"NO LOCATOR" if not found_ok(row) else "locators=%d" % locators(row)}'
              f'{"  native " + row["native"]["name"] if row.get("native") else ""}{"  near %#x%+#x" % (row["near"]["rva"], row["near"]["delta"]) if row.get("near") else ""}'
              f'{"  reference %d/%d to %#x" % (row["xref_of"]["ordinal"] + 1, row["xref_of"]["of"], row["xref_of"]["target"]) if row.get("xref_of") else ""}'
              f'{"  vtable %#x+%#x" % (row["vslot"]["table"], row["vslot"]["delta"]) if row.get("vslot") else ""}'
              f'{"  inside %#x+%#x" % (row["inside"]["base"], row["inside"]["delta"]) if row.get("inside") else ""}'
              f'  {row["uses"][0][0]}:{row["uses"][0][1]}')
    DATA.mkdir(parents=True, exist_ok=True)
    SIGS.write_text(json.dumps(dict(exe=str(img.path), sha256=img.sha256, bytes=len(img.raw), timestamp=img.timestamp, image_size=img.size, entry=img.entry,
                                    sections=img.sections, addresses=rows), indent=1), encoding='utf-8')
    single = sum(1 for r in rows if r['kind'] not in ('bound', 'ignore') and locators(r) == 1)
    print(f'{len(rows)} pinned addresses, {sum(not found_ok(r) for r in rows)} without a locator, {single} with only one -> {SIGS}')


def locate(args):
    base = json.loads(SIGS.read_text(encoding='utf-8')); img = Image(args.exe); s, code = img.code(); va = s[1]
    image_base = pointers(img)[1]
    print(f'new exe sha256={img.sha256} bytes={len(img.raw)} timestamp={img.timestamp:#x} image={img.size:#x} entry={img.entry:#x}'
          f'{"  (unchanged)" if img.sha256 == base["sha256"] else ""}')

    def by_sites(listed):
        targets = []
        for x in listed:
            h = hits(code, parse(x['signature']))
            if len(h) != 1: continue
            at = va + h[0] + x['offset']; insn = next(md.disasm(img.bytes_at(at, 16), at, 1), None)
            if not insn: continue
            if insn.disp_size: targets.append(insn.address + insn.size + insn.disp)
            else: targets += [op.imm for op in insn.operands if op.type == capstone.x86.X86_OP_IMM][:1]  # call / jmp rel32
        return (targets[0], '') if targets and len(set(targets)) == 1 else (None, f'references point at {sorted(map(hex, set(targets)))}')

    def in_code(rva):
        return s[1] <= rva < s[1] + max(s[2], s[4])

    def slot_function(slot):
        f = int.from_bytes(img.bytes_at(slot, 8), 'little') - image_base
        return f if in_code(f) else None

    rows = []
    for row in base['addresses']:
        r = dict(rva=row['rva'], kind=row['kind'], uses=row['uses'], found=[], notes=[], snap=row, used=set())
        if row['kind'] == 'ignore': r['found'].append(('fixed', row['rva']))
        elif row['kind'] == 'bound': r['found'].append(('section end', (s[1] + s[2]) & ~0xfffff))  # rounded down to a MiB
        else:
            if row.get('signature'):
                h = hits(code, parse(row['signature']))
                if len(h) == 1: r['found'].append(('own bytes', va + h[0]))
                else: r['notes'].append(f'own bytes {len(h)} matches')
            if row.get('native'):
                f = native_find(img, row['native'])
                if f is not None: r['found'].append(('native table', f))
                else: r['notes'].append(f'native {row["native"]["name"]} not found / ambiguous')
            if row.get('xrefs'):
                t, note = by_sites(row['xrefs'])
                if t is not None: r['found'].append(('references', t))
                else: r['notes'].append(note)
            v = row.get('vslot')
            if v and v.get('sites'):
                t, note = by_sites(v['sites']); f = slot_function(t + v['delta']) if t is not None else None
                if f is not None: r['found'].append(('vtable slot', f))
                else: r['notes'].append('vtable ' + (note or 'slot holds no code'))
        rows.append(r)
    index = {r['rva']: r for r in rows}

    def settled(r):
        values = {f[1] for f in r['found']}
        return next(iter(values)) if len(values) == 1 else None

    # Locators that go through another pin (its function, its vtable, its global), in rounds: that pin may be found
    # the same way. The neighbour distance between globals comes last, only where nothing else found the pin.
    for _ in range(3):
        for r in rows:
            snap = r['snap']
            v = snap.get('vslot')
            if v and not v.get('sites') and 'vslot' not in r['used'] and v['table'] in index and settled(index[v['table']]) is not None:
                r['used'].add('vslot'); f = slot_function(settled(index[v['table']]) + v['delta'])
                if f is not None: r['found'].append(('vtable slot', f))
                else: r['notes'].append('vtable slot holds no code')
            x = snap.get('inside')
            if x and 'inside' not in r['used'] and x['base'] in index and settled(index[x['base']]) is not None:
                r['used'].add('inside'); at = settled(index[x['base']]) + x['delta']; pattern = parse(x['pattern'])
                if [b for b in masked(img, at, len(pattern))[:len(pattern)]] == pattern: r['found'].append(('inside its function', at))
                else: r['notes'].append(f'inside {x["base"]:#x}: the bytes there changed')
            x = snap.get('xref_of')
            if x and 'xref_of' not in r['used'] and x['target'] in index and settled(index[x['target']]) is not None:
                r['used'].add('xref_of'); n = len(x['pattern'].split())
                same = [end - n for end in sorted({at + size for at, size in xrefs(img, settled(index[x['target']]))}) if text(masked(img, end - n, n)[:n]) == x['pattern']]
                if len(same) == x['of']: r['found'].append((f'reference {x["ordinal"] + 1}/{x["of"]} to its global', same[x['ordinal']]))
                else: r['notes'].append(f'{len(same)} matching references to its global (snapshot {x["of"]})')
    for r in rows:
        near = r['snap'].get('near')
        if not r['found'] and near and near['rva'] in index and settled(index[near['rva']]) is not None:
            r['found'].append((f'neighbour {near["rva"]:#x} + fixed distance: verify in game', settled(index[near['rva']]) + near['delta']))
    out = []; bad = 0
    for r in rows:
        values = {f[1] for f in r['found']}; new = next(iter(values)) if len(values) == 1 else None
        why = ' + '.join(f[0] for f in r['found']) if new is not None else ('DISAGREE ' + ', '.join(f'{f[0]} {f[1]:#x}' for f in r['found']) if r['found'] else '')
        if r['notes']: why = (why + '; ' if why else '') + '; '.join(r['notes'])
        if new is None: bad += 1
        out.append(dict(rva=r['rva'], new=new, kind=r['kind'], uses=r['uses'], why=why))
        print(f'{r["rva"]:#010x} -> {"MISSING" if new is None else format(new, "#010x")} {r["kind"]:9s} {why}  {r["uses"][0][0]}:{r["uses"][0][1]}')
    target = Path(args.out) if args.out else MOVED
    result = dict(exe=str(img.path), sha256=img.sha256, bytes=len(img.raw), timestamp=img.timestamp, image_size=img.size, entry=img.entry,
                  missing=bad, addresses=out)
    target.write_text(json.dumps(result, indent=1), encoding='utf-8')
    print(f'{len(out) - bad} of {len(out)} found, {bad} missing -> {target}')
    return result


def apply(args):
    moved = json.loads(MOVED.read_text(encoding='utf-8'))
    if moved['missing'] and not args.force: raise SystemExit(f'{moved["missing"]} addresses missing: find them by hand first (or --force to write the rest)')
    # Where each address is used now (the snapshot's line numbers go stale as soon as the sources change).
    edits = {}; now = pinned()
    olds = {r['rva'] for r in moved['addresses']}; clash = {r['new'] for r in moved['addresses'] if r['new'] is not None and r['new'] != r['rva']} & olds
    if clash: raise SystemExit(f'new addresses equal to old ones {sorted(map(hex, clash))}: apply them by hand')
    for row in moved['addresses']:
        if row['new'] is None or row['new'] == row['rva']: continue
        for file, line, literal in now.get(row['rva'], []): edits.setdefault(file, []).append((line, literal, row['new']))
    proxy = ROOT/'src/core/proxy.cpp'; ptext = proxy.read_bytes().decode('utf-8')
    for name, value in (('kTimestamp', f'{moved["timestamp"]:#010x}'), ('kImageSize', f'{moved["image_size"]:#010x}'), ('kEntrypoint', f'{moved["entry"]:#010x}'),
                        ('kExeBytes', str(moved['bytes'])), ('kSha256', f'"{moved["sha256"]}"')):
        ptext, n = re.subn(rf'(constexpr [^=;]*?\b{name}\s*=\s*)[^;]+;', lambda m: m.group(1) + value + ';', ptext)
        print(f'proxy.cpp {name} = {value} ({n})')
        if n != 1: raise SystemExit(f'proxy.cpp: {name} matched {n} times; fix the pin by hand')
    proxy.write_bytes(ptext.encode('utf-8'))
    # Code self-checks at pinned addresses: the new exe's bytes at the new address, token by token (layout kept). Only
    # where the code itself is unchanged (bytes differ only in displacements, branch targets or past a ret / jmp);
    # otherwise the check is left failing, so that feature stays off until someone looks.
    img = Image(moved['exe']); table = {r['rva']: r['new'] for r in moved['addresses'] if r['new'] is not None}; changed = []
    for file, arrays in groupby(sorted((a for a in code_arrays() if a['address'] is not None), key=lambda a: a['file']), key=lambda a: a['file']):
        path = ROOT/file; text = path.read_bytes().decode('utf-8'); out = text; shift = 0
        for a in sorted(arrays, key=lambda a: a['tokens'][0][0]):
            new = table.get(a['address']); label = f'{file}:{a["line"]} {a["name"] or "row"} at {a["address"]:#x}'
            if new is None: print(f'{label}: address not relocated, left'); continue
            old = bytes(t[3] for t in a['tokens']); fresh = img.bytes_at(new, len(old)); free = variable(img, new, len(old))
            if any(o != f and not v for o, f, v in zip(old, fresh, free)): changed.append(label); print(f'{label}: CODE CHANGED, left'); continue
            if old == fresh: continue
            for (start, end, tok, _), b in zip(a['tokens'], fresh):
                repl = str(b) if not tok.startswith('0x') else (f'0x{b:02x}' if tok[2:].lower() == tok[2:] else f'0x{b:02X}')
                out = out[:start + shift] + repl + out[end + shift:]; shift += len(repl) - (end - start)
            print(f'{label}: bytes refreshed')
        if out != text: path.write_bytes(out.encode('utf-8'))
    for file, rows in edits.items():
        path = ROOT/file; raw = path.read_bytes().decode('utf-8'); crlf = '\r\n' in raw; lines = raw.replace('\r\n', '\n').split('\n')
        for line, literal, new in rows:
            width = len(literal) - 2; repl = f'0x{new:0{width}x}' if literal[2:].islower() or not literal[2:].isalpha() else f'0x{new:0{width}X}'
            assert literal in lines[line - 1], (file, line, literal)
            lines[line - 1] = lines[line - 1].replace(literal, repl); print(f'{file}:{line} {literal} -> {repl}')
        out = '\n'.join(lines); path.write_bytes((out.replace('\n', '\r\n') if crlf else out).encode('utf-8'))


def protector(img):
    """The code protector's section: executable and writable (0xe0000020), the same VA in every build so far."""
    return next((s for s in img.sections if s[5] & 0xe0000020 == 0xe0000020), None)


def protector_jump(img, data, address, at):
    """The call / jmp / jcc rel32 in `data` (code at `address`) whose displacement holds byte `at`, when its target lies
    in img's protector section: (first, end) of the displacement; else None."""
    p = protector(img)
    for insn in md.disasm(bytes(data) + b'\x90' * 16, address):
        if insn.address > at: break
        lo = insn.address + insn.imm_offset
        if insn.imm_size == 4 and (insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL)) and lo <= at < lo + 4:
            target = next((op.imm for op in insn.operands if op.type == capstone.x86.X86_OP_IMM), None)
            return (lo, lo + 4) if p and target is not None and p[1] <= target < p[1] + max(p[2], p[4]) else None
    return None


def build(args):
    """Another shipping build of the same changelist (BUILDS): the pins must all sit at the primary build's RVAs; the
    self-checks at pins whose bytes there differ only by jumps into the protector section get rows in game_build.cpp;
    proxy.cpp gets the build's exe facts. Anything else is reported, never guessed."""
    if args.name not in BUILDS: raise SystemExit(f'unknown build {args.name!r}; known: {", ".join(BUILDS)}')
    spec = BUILDS[args.name]; img = Image(args.exe)
    # The Steam image must be the verified baseline, not a developer's fixed installation path.
    baseline = json.loads(SIGS.read_text(encoding='utf-8'))
    primary = Image(args.primary_exe or baseline['exe'])
    if primary.sha256 != proxy_facts()['sha256'] or primary.sha256 != baseline['sha256']:
        raise SystemExit('primary EXE does not match proxy.cpp and the signature baseline')
    if img.sha256 == proxy_facts()['sha256']: raise SystemExit('that is the primary (Steam) exe; give the other build with --exe')
    moved = locate(argparse.Namespace(exe=args.exe, out=str(DATA/f'exe_relocated_{args.name}.json')))
    pins = pinned(); found = {r['rva']: r['new'] for r in moved['addresses']}; bad = []
    for v in sorted(pins):
        if v not in found: bad.append(f'{v:#x} has no signature (run snapshot on the primary build first)')
        elif found[v] is None: bad.append(f'{v:#x} MISSING on {args.name}')
        elif found[v] != v: bad.append(f'{v:#x} is at {found[v]:#x} on {args.name}')
    for b in bad: print('PIN', b)
    if bad: raise SystemExit(f'{len(bad)} pin(s) differ on {args.name}: it would need its own pin set, which the DLL does not have; nothing written')
    rows, changed = {}, []
    for a in code_arrays(pins):
        if a['address'] is None: continue
        want = bytes(t[3] for t in a['tokens']); got = img.bytes_at(a['address'], len(want)); label = f'{Path(a["file"]).name}:{a["line"]} {a["name"] or "row"}'
        if want == got: continue
        spans = set()
        for i in (i for i in range(len(want)) if want[i] != got[i]):
            s1 = protector_jump(primary, want, a['address'], a['address'] + i); s2 = protector_jump(img, got, a['address'], a['address'] + i)
            if not s1 or s1 != s2: spans = None; break
            spans.add(s1)
        if spans is None:
            changed.append(label); print(f'CODE CHANGED {label} at {a["address"]:#x}\n    source {want.hex(" ")}\n    {args.name:6s} {got.hex(" ")}'); continue
        for lo, hi in sorted(spans):
            lo, hi = max(lo, a['address']), min(hi, a['address'] + len(want)); k, e = lo - a['address'], hi - a['address']
            row = rows.setdefault((a['address'], k), dict(pin=a['address'], offset=k, steam=want[k:e], other=got[k:e], labels=[]))
            if (row['steam'], row['other']) != (want[k:e], got[k:e]): raise SystemExit(f'{label}: bytes at {lo:#x} disagree with another self-check')
            row['labels'].append(label)
    # game_build.cpp: this build's rows replaced, other builds' rows and the empty end row kept.
    raw = GAME_BUILD.read_bytes().decode('utf-8'); crlf = '\r\n' in raw; lines = raw.replace('\r\n', '\n').split('\n')
    block = sorted(build_byte_lines(lines))
    if not block: raise SystemExit(f'{GAME_BUILD}: no exe-build-bytes table')
    hexes = lambda b: ','.join(f'0x{x:02x}' for x in b)
    fresh = [f'    {{Id::{spec["id"]}, {r["pin"]:#x}, {r["offset"]}, {len(r["steam"])}, {{{hexes(r["steam"])}}}, {{{hexes(r["other"])}}}}},  // {", ".join(r["labels"])}'
             for _, r in sorted(rows.items())]
    body = [lines[n - 1] for n in block[1:-1] if not (BUILD_ROW.match(lines[n - 1]) and BUILD_ROW.match(lines[n - 1]).group(1) == spec['id'])]
    end = next((i for i, line in enumerate(body) if re.match(r'^\s*\{\},', line)), None)
    if end is None: raise SystemExit(f'{GAME_BUILD}: the table has no empty end row')
    lines[block[0]:block[-1] - 1] = body[:end] + fresh + body[end:]
    out = '\n'.join(lines); GAME_BUILD.write_bytes((out.replace('\n', '\r\n') if crlf else out).encode('utf-8'))
    for row in fresh: print('ROW', row.strip())
    # proxy.cpp: the build's exe facts.
    proxy = SRC/'core'/'proxy.cpp'; ptext = proxy.read_bytes().decode('utf-8')
    for key, value in (('Timestamp', f'{img.timestamp:#010x}'), ('ImageSize', f'{img.size:#010x}'), ('Entrypoint', f'{img.entry:#010x}'),
                       ('ExeBytes', str(len(img.raw))), ('Sha256', f'"{img.sha256}"')):
        name = f'k{spec["prefix"]}{key}'
        ptext, n = re.subn(rf'(constexpr [^=;]*?\b{name}\s*=\s*)[^;]+;', lambda m: m.group(1) + value + ';', ptext)
        if n != 1: raise SystemExit(f'proxy.cpp: {name} matched {n} times; add the constant by hand')
        print(f'proxy.cpp {name} = {value}')
    proxy.write_bytes(ptext.encode('utf-8'))
    print(f'{args.name}: {len(pins)} pins at the same RVAs, {len(fresh)} byte row(s), {len(changed)} self-check(s) with changed code')
    if changed: raise SystemExit(f'{len(changed)} self-check(s) differ by more than protector jumps on {args.name}: their features stay off there until someone looks')


def audit(args):
    img = Image(args.exe); s, code = img.code(); pins = pinned(); problems = 0
    primary_facts = proxy_facts(); proxy_sha = primary_facts['sha256']
    other = next((n for n, b in BUILDS.items() if proxy_facts(b['prefix'])['sha256'] == img.sha256), None)
    bid = BUILDS[other]['id'] if other else None; byte_rows = build_rows()
    sigs = json.loads(SIGS.read_text(encoding='utf-8')) if SIGS.exists() else None; same = img.sha256 == proxy_sha
    print(f'exe {img.sha256[:12]} ({"the build the DLL pins" if same else f"the {other} build the DLL accepts" if other else "NOT a build the DLL accepts, pins " + proxy_sha[:12]}), '
          f'snapshot {sigs["sha256"][:12] if sigs else "none"}, {len(pins)} pinned addresses')
    # The header facts proxy.cpp compares before it hashes the file.
    facts = primary_facts if same else proxy_facts(BUILDS[other]['prefix']) if other else None
    for key, value in (('timestamp', img.timestamp), ('image_size', img.size), ('entry', img.entry), ('bytes', len(img.raw))) if facts else ():
        if facts[key] != value: problems += 1; print(f'PROXY {key} {facts[key]} != exe {value}')
    # Another build: the DLL uses the primary build's pins, so every one must sit at the same RVA there.
    if other:
        path = DATA/f'exe_relocated_{other}.json'; moved = json.loads(path.read_text(encoding='utf-8')) if path.exists() else None
        if not moved or moved['sha256'] != img.sha256: problems += 1; print(f'NO RELOCATION of this exe: run build {other}')
        else:
            found = {r['rva']: r['new'] for r in moved['addresses']}
            for v in sorted(pins):
                if found.get(v, -1) != v: problems += 1; print(f'PIN {v:#x} is {found.get(v)!r} on {other}  {pins[v][0][0]}:{pins[v][0][1]}  (run build {other})')
    # Per-build byte rows: Steam bytes are the primary exe's, the build's bytes its exe's, and a self-check uses each.
    at_pins = {a['address'] for a in code_arrays(pins) if a['address'] is not None}
    for r in byte_rows:
        at = r['pin'] + r['offset']; where = f'{r["build"]} row {r["pin"]:#x}+{r["offset"]}'
        if r['pin'] not in at_pins: problems += 1; print(f'UNUSED {where}: no self-check at that pin')
        if same and img.bytes_at(at, len(r['steam'])) != r['steam']: problems += 1; print(f'STALE {where}: Steam bytes differ from the exe (run build {r["build"].lower()})')
        if other and r['build'] == bid and img.bytes_at(at, len(r['other'])) != r['other']: problems += 1; print(f'STALE {where}: {other} bytes differ from the exe (run build {other})')
    # Every pin has a signature taken from the pinned build, so the next update can find it.
    if sigs and sigs['sha256'] == proxy_sha:
        rows = {r['rva']: r for r in sigs['addresses']}
        for v in sorted(v for v in pins if v not in rows or not found_ok(rows[v])):
            problems += 1; print(f'NO SIGNATURE {v:#x}  {pins[v][0][0]}:{pins[v][0][1]}  (run snapshot)')
    else: problems += 1; print('SNAPSHOT is not of the pinned build: run snapshot')
    # Code-byte self-checks: at a pin they must equal the pinned build's bytes; against code found at runtime they must
    # still occur somewhere in the code section (else that check fails in game and its feature stays off).
    arrays = code_arrays(pins); runtime = 0
    for a in arrays:
        data = bytes(t[3] for t in a['tokens']); label = f'{a["file"]}:{a["line"]} {a["name"] or "row"}'
        if a['address'] is not None:
            if not same and not other: continue
            # What GameBuild::CodeMatches compares on this build: the source bytes, with the build's rows swapped in.
            got = img.bytes_at(a['address'], len(data)); want = data if same else with_rows(data, a['address'], bid, byte_rows)
            if got != want: problems += 1; print(f'DIFFERS {label} at {a["address"]:#x}{"" if same else " on " + other}\n    source {data.hex(" ")}'
                                                  f'{"" if same or want is None else chr(10) + "    rows   " + want.hex(" ")}{" (stale row)" if want is None else ""}\n    exe    {got.hex(" ")}')
        elif len(data) >= 5:
            runtime += 1
            if not hits(code, list(data), 1): problems += 1; print(f'NOT IN EXE {label}: {len(data)} bytes compared with code found at runtime')
    print(f'{sum(a["address"] is not None for a in arrays)} self-checks at pins{"" if same or other else " (not compared: other build)"}, '
          f'{runtime} against runtime code, {sum(r["build"] == bid for r in byte_rows) if other else len(byte_rows)} per-build byte row(s)')
    # Address-sized literals in code (comments aside) that no pin rule covers: a new pin the scanner would miss.
    for path in sources():
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines(); pinl = pin_lines(lines)
        for n, line in enumerate(lines, 1):
            body = line.split('//')[0]
            if n in pinl or PROXY_PINS.search(body): continue
            for m in LITERAL.finditer(body):
                v = int(m.group(0), 16)
                if 0x10000 <= v < img.size and v not in NOT_ADDRESSES and img.section(v):
                    problems += 1; print(f'UNCLASSIFIED {m.group(0)}  {path.relative_to(ROOT)}:{n}  {body.strip()[:100]}')
    print(f'{problems} problem(s)')
    if problems: raise SystemExit(1)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('cmd', choices=('snapshot', 'locate', 'apply', 'audit', 'build'))
    ap.add_argument('name', nargs='?', help='build: the other build (' + ', '.join(BUILDS) + ')')
    ap.add_argument('--exe', help='PBZ.exe to inspect; required except for apply, which uses the relocation record')
    ap.add_argument('--primary-exe', help='build: Steam EXE (default: the path recorded by snapshot)')
    ap.add_argument('--data-dir', type=Path, default=DATA, help='private signature/relocation directory (default: local/data)')
    ap.add_argument('--force', action='store_true'); ap.add_argument('--refresh', action='store_true', help='snapshot: redo every signature')
    ap.add_argument('--out', help='locate: write the result here instead of local/data/exe_relocated.json')
    a = ap.parse_args()
    if a.cmd == 'build' and a.name not in BUILDS: ap.error(f'build needs one of: {", ".join(BUILDS)}')
    if a.cmd != 'apply' and not a.exe: ap.error('--exe is required for ' + a.cmd)
    DATA = a.data_dir.resolve()
    SIGS, MOVED = DATA/'exe_signatures.json', DATA/'exe_relocated.json'
    {'snapshot': snapshot, 'locate': locate, 'apply': apply, 'audit': audit, 'build': build}[a.cmd](a)
