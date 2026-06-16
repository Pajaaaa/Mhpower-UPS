import zipfile, sys, struct

path = sys.argv[1] if len(sys.argv) > 1 else '/home/paja/data.sr'
z = zipfile.ZipFile(path)
names = [n for n in z.namelist() if n.startswith('logic-1-')]
# order chunks numerically
def idx(n): return int(n.split('-')[-1])
names.sort(key=idx)
data = b''.join(z.read(n) for n in names)
print("total samples:", len(data), "files:", names)

# per-channel transition counts and high-fraction
trans = [0]*8
high = [0]*8
prev = data[0]
for b in data:
    diff = b ^ prev
    for ch in range(8):
        if diff & (1<<ch):
            trans[ch] += 1
        if b & (1<<ch):
            high[ch] += 1
    prev = b
n = len(data)
print("ch :  transitions   high%")
for ch in range(8):
    print(f"D{ch}: {trans[ch]:10d}   {100*high[ch]/n:6.1f}%")
