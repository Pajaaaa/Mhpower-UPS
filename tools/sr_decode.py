import zipfile, sys

path = sys.argv[1] if len(sys.argv) > 1 else '/home/paja/data.sr'
z = zipfile.ZipFile(path)
names = sorted([n for n in z.namelist() if n.startswith('logic-1-')], key=lambda n: int(n.split('-')[-1]))
data = b''.join(z.read(n) for n in names)
N = len(data)

# active window
def active_range():
    lo, hi = None, None
    prev = data[0]
    for i, b in enumerate(data):
        if (b ^ prev) & 0b11:
            if lo is None: lo = i
            hi = i
        prev = b
    return lo, hi
lo, hi = active_range()
print(f"active window: samples {lo}..{hi}  span={hi-lo} ( {(hi-lo)/20000*1000:.1f} ms @20kHz )")

# 7-seg pattern -> digit (from firmware digitFromPattern)
SEG = {0x77:'0',0x41:'1',0x6E:'2',0x6D:'3',0x59:'4',0x3D:'5',0x3F:'6',0x61:'7',0xFF:'8',0x00:' '}
def dig(p): return SEG.get(p, '?')

def decode(clk_ch, din_ch, rising, lsb_first, invert):
    bits = []
    prev = (data[0] >> clk_ch) & 1
    for i in range(1, N):
        c = (data[i] >> clk_ch) & 1
        hit = (not prev and c) if rising else (prev and not c)
        if hit:
            d = (data[i] >> din_ch) & 1
            if invert: d ^= 1
            bits.append(d)
        prev = c
    out = []
    for i in range(0, len(bits) - 7, 8):
        v = 0
        if lsb_first:
            for n in range(8): v |= bits[i+n] << n
        else:
            for n in range(8): v = (v<<1) | bits[i+n]
        out.append(v)
    return bits, out

def find_frame(d):
    for i in range(len(d)-12):
        if d[i]==0x40 and d[i+1]==0xC0 and (d[i+12]&0xF0)==0x80:
            return d[i+2:i+12], d[i+12], i, '40 C0'
    for i in range(len(d)-11):
        if d[i]==0xC0 and (d[i+11]&0xF0)==0x80:
            return d[i+1:i+11], d[i+11], i, 'C0'
    return None

best = None
for clk_ch, din_ch in ((0,1),(1,0)):
    for rising in (True, False):
        for lsb in (True, False):
            for inv in (False, True):
                bits, out = decode(clk_ch, din_ch, rising, lsb, inv)
                fr = find_frame(out)
                score = len(out)
                if fr:
                    print(f"\n*** FRAME clk=D{clk_ch} din=D{din_ch} edge={'rise' if rising else 'fall'} "
                          f"{'LSB' if lsb else 'MSB'} inv={inv} bits={len(bits)} bytes={len(out)} hdr={fr[3]}")
                    mem, bright, pos = fr[0], fr[1], fr[2]
                    print("  raw bytes:", ' '.join(f'{b:02X}' for b in out))
                    print("  mem[0..9]:", ' '.join(f'{b:02X}' for b in mem), f"  brightness=0x{bright:02X}")
                    vin = ''.join(dig(mem[i]) for i in range(3))
                    vout = ''.join(dig(mem[i]) for i in range(3,6))
                    print(f"  INPUT (mem0-2)  = '{vin}' V")
                    print(f"  OUTPUT(mem3-5)  = '{vout}' V")
                    print(f"  mode mem[6]=0x{mem[6]:02X}  load mem[7]=0x{mem[7]:02X}  mem[8]=0x{mem[8]:02X}  battbars mem[9]=0x{mem[9]:02X}")
                    best = fr
if best is None:
    print("\nNo valid frame found in any variant.")
