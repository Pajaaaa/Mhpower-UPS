import zipfile, sys
path = sys.argv[1] if len(sys.argv) > 1 else '/home/paja/data.sr'
z = zipfile.ZipFile(path)
names = sorted([n for n in z.namelist() if n.startswith('logic-1-')], key=lambda n: int(n.split('-')[-1]))
data = b''.join(z.read(n) for n in names)
N = len(data)

# edge indices for D0 and D1
for ch in (0,1):
    edges=[]
    prev=(data[0]>>ch)&1
    for i in range(1,N):
        c=(data[i]>>ch)&1
        if c!=prev:
            edges.append(i)
        prev=c
    print(f"\n=== D{ch}: {len(edges)} edges ===")
    # gaps between consecutive edges
    gaps=[edges[i+1]-edges[i] for i in range(len(edges)-1)]
    if gaps:
        sg=sorted(gaps)
        print(f"  gap min={min(gaps)} median={sg[len(sg)//2]} max={max(gaps)}")
    # show first 40 edge indices
    print("  first edges:", edges[:40])
    # cluster edges into bursts (gap>50 samples => new burst)
    bursts=[]
    start=edges[0]; last=edges[0]; cnt=1
    for e in edges[1:]:
        if e-last>50:
            bursts.append((start,last,cnt))
            start=e; cnt=0
        last=e; cnt+=1
    bursts.append((start,last,cnt))
    print(f"  {len(bursts)} bursts (gap>50). first 10:")
    for b in bursts[:10]:
        print(f"    [{b[0]}..{b[1]}] len={b[1]-b[0]} edges={b[2]}")
