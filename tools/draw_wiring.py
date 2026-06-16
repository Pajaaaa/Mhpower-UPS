import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyBboxPatch, Polygon, Circle
from matplotlib.lines import Line2D

fig, ax = plt.subplots(figsize=(14, 9.5), dpi=130)
ax.set_xlim(0, 14); ax.set_ylim(0, 9.5); ax.axis("off")

LW = 2.0
def wire(pts, color="black", lw=LW):
    xs=[p[0] for p in pts]; ys=[p[1] for p in pts]
    ax.add_line(Line2D(xs, ys, color=color, lw=lw, solid_capstyle="round"))

def node(x,y,r=0.05):
    ax.add_patch(Circle((x,y), r, color="black", zorder=5))

def box(x,y,w,h,title,sub="",fc="#f4f6f8",ec="#222"):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.02,rounding_size=0.12",
                 fc=fc, ec=ec, lw=2))
    ax.text(x+w/2, y+h-0.32, title, ha="center", va="top", fontsize=12, fontweight="bold")
    if sub:
        ax.text(x+w/2, y+h-0.7, sub, ha="center", va="top", fontsize=8.5, color="#555")

def res(x,y,label,horiz=True,l=0.9,h=0.28):
    if horiz:
        ax.add_patch(Rectangle((x,y-h/2), l, h, fc="white", ec="black", lw=2))
        ax.text(x+l/2, y+h/2+0.12, label, ha="center", va="bottom", fontsize=9)
        return (x, y), (x+l, y)
    else:
        ax.add_patch(Rectangle((x-h/2,y), h, l, fc="white", ec="black", lw=2))
        ax.text(x+h/2+0.12, y+l/2, label, ha="left", va="center", fontsize=9)
        return (x, y), (x, y+l)

def inverter(cx, cy, label):
    # triangle pointing right + bubble
    tri=[(cx-0.42,cy+0.38),(cx-0.42,cy-0.38),(cx+0.34,cy)]
    ax.add_patch(Polygon(tri, closed=True, fc="#eaf2ff", ec="black", lw=2, zorder=3))
    ax.add_patch(Circle((cx+0.43,cy),0.09, fc="white", ec="black", lw=2, zorder=4))
    ax.text(cx-0.05, cy-0.72, label, ha="center", va="top", fontsize=8.5, color="#1a3a6b")
    return (cx-0.42, cy), (cx+0.52, cy)  # input point, output point

def gnd(x,y,scale=1.0):
    s=0.14*scale
    wire([(x,y),(x,y-0.18)])
    for i,wdt in enumerate([0.30,0.19,0.09]):
        yy=y-0.18-i*0.10
        ax.add_line(Line2D([x-wdt/2,x+wdt/2],[yy,yy],color="black",lw=2))

# ---------------- Title ----------------
ax.text(7, 9.25, "MHpower – správné zapojení 74LVC14A (level shifter 5 V → 3,3 V)",
        ha="center", va="top", fontsize=14, fontweight="bold")
ax.text(7, 8.85, "Použita jen 2 hradla ze 6. Signály vyjdou invertované – firmware si to najde sám (edge+invert).",
        ha="center", va="top", fontsize=9.5, color="#555")

# ---------------- Boxes ----------------
box(0.4, 4.6, 2.3, 3.0, "Displej\nTM1640", fc="#f0f0f0")
box(4.5, 4.0, 4.6, 4.0, "74LVC14A", "(Schmitt invertor, 6 hradel)", fc="#fbfbfb")
box(11.4, 4.4, 2.2, 3.2, "ESP32", fc="#f0f0f0")

# TM1640 pins (right edge x=2.7)
clk_y, din_y, v5_y, gnd_y = 7.0, 6.2, 5.4, 4.9
for lbl,yy in [("CLK",clk_y),("DIN",din_y),("VCC 5 V",v5_y),("GND",gnd_y)]:
    ax.text(2.55, yy, lbl, ha="right", va="center", fontsize=10, fontweight="bold")
    node(2.7, yy)

# ESP pins (left edge x=11.4)
e3v3_y, g18_y, g23_y, egnd_y = 7.0, 6.3, 5.5, 4.8
for lbl,yy in [("3V3",e3v3_y),("GPIO18",g18_y),("GPIO23",g23_y),("GND",egnd_y)]:
    ax.text(11.55, yy, lbl, ha="left", va="center", fontsize=10, fontweight="bold")
    node(11.4, yy)

# inverters inside chip
invA_in, invA_out = inverter(6.7, clk_y, "hradlo 1: pin 1 → pin 2")
invB_in, invB_out = inverter(6.7, din_y, "hradlo 2: pin 3 → pin 4")

# ---------------- CLK path ----------------
a,b = res(3.2, clk_y, "1 kΩ")
wire([(2.7,clk_y),(3.2,clk_y)])
wire([(b[0],clk_y),(invA_in[0],clk_y)])
ax.text(5.6, clk_y+0.20, "pin 1", ha="center", fontsize=7.5, color="#777")
# output -> 100R -> GPIO18
c,d = res(9.55, clk_y, "100 Ω")
wire([(invA_out[0],clk_y),(9.55,clk_y)])
ax.text(7.35, clk_y+0.20, "pin 2", ha="center", fontsize=7.5, color="#777")
wire([(d[0],clk_y),(10.8,clk_y),(10.8,g18_y),(11.4,g18_y)])

# ---------------- DIN path ----------------
a,b = res(3.2, din_y, "1 kΩ")
wire([(2.7,din_y),(3.2,din_y)])
wire([(b[0],din_y),(invB_in[0],din_y)])
ax.text(5.6, din_y+0.20, "pin 3", ha="center", fontsize=7.5, color="#777")
c,d = res(9.55, din_y, "100 Ω")
wire([(invB_out[0],din_y),(9.55,din_y)])
ax.text(7.35, din_y+0.20, "pin 4", ha="center", fontsize=7.5, color="#777")
wire([(d[0],din_y),(10.6,din_y),(10.6,g23_y),(11.4,g23_y)])

# ---------------- Power: 3V3 ----------------
vcc_rail_y = 8.25
wire([(11.4,e3v3_y),(11.0,e3v3_y),(11.0,vcc_rail_y),(6.0,vcc_rail_y)], color="#c0392b")
wire([(6.0,vcc_rail_y),(6.0,8.0)], color="#c0392b")  # down into chip VCC (pin14)
ax.text(6.05, 8.05, "pin 14 (VCC)", ha="left", va="bottom", fontsize=7.5, color="#c0392b")
ax.text(8.6, vcc_rail_y+0.12, "3,3 V", ha="center", fontsize=9, color="#c0392b", fontweight="bold")
# decoupling cap 100nF between VCC rail and GND
capx=5.0
wire([(6.0,vcc_rail_y),(capx,vcc_rail_y),(capx,7.7)], color="#c0392b")
ax.add_line(Line2D([capx-0.18,capx+0.18],[7.7,7.7],color="black",lw=2.5))
ax.add_line(Line2D([capx-0.18,capx+0.18],[7.55,7.55],color="black",lw=2.5))
ax.text(capx+0.25, 7.62, "100 nF", ha="left", va="center", fontsize=8)
wire([(capx,7.55),(capx,3.4)])  # cap to gnd rail

# ---------------- GND rail ----------------
gnd_rail_y = 3.4
wire([(1.4,gnd_y),(1.4,gnd_rail_y),(12.4,gnd_rail_y)])   # TM1640 GND down + rail
wire([(2.7,gnd_y),(1.4,gnd_y)])
wire([(11.4,egnd_y),(12.4,egnd_y),(12.4,gnd_rail_y)])    # ESP GND
# chip GND pin7
wire([(6.7,4.0),(6.7,gnd_rail_y)])
ax.text(6.75, 3.95, "pin 7 (GND)", ha="left", va="top", fontsize=7.5)
gnd(7.6, gnd_rail_y)
# unused inputs to GND
ax.text(8.95, 4.55, "Nepoužité vstupy 5, 9, 11, 13 → GND", ha="left", va="center",
        fontsize=8, color="#1a6b2f", fontweight="bold")
wire([(8.9,4.55),(8.65,4.55),(8.65,gnd_rail_y)], color="#1a6b2f")

ax.text(0.6, gnd_rail_y-0.55, "Společná zem: GND displeje + GND ESP32 + pin 7 čipu. Napájení displeje zůstává 5 V.",
        ha="left", va="top", fontsize=9, color="#333")

# ---------------- Connection table ----------------
tbl = [
 "PŘIPOJENÍ PIN PO PINU:",
 "  TM1640 CLK   → 1 kΩ → 74LVC14 pin 1 (1A)",
 "  74LVC14 pin 2 (1Y) → 100 Ω → ESP32 GPIO18",
 "  TM1640 DIN   → 1 kΩ → 74LVC14 pin 3 (2A)",
 "  74LVC14 pin 4 (2Y) → 100 Ω → ESP32 GPIO23",
 "  74LVC14 pin 14 (VCC) → ESP32 3V3   |   pin 7 (GND) → GND",
 "  100 nF mezi pin 14 a GND (co nejblíž čipu)",
 "  Nepoužité VSTUPY 5, 9, 11, 13 → GND   |   výstupy 6, 8, 10, 12 = nezapojeno",
]
ax.add_patch(FancyBboxPatch((0.4,0.25),13.2,2.55,boxstyle="round,pad=0.05,rounding_size=0.1",
             fc="#fffef2", ec="#caa", lw=1.5))
for i,line in enumerate(tbl):
    ax.text(0.65, 2.55-i*0.30, line, ha="left", va="top",
            fontsize=10.5 if i==0 else 10,
            fontweight="bold" if i==0 else "normal",
            family="monospace", color="#222")

plt.tight_layout()
plt.savefig("/home/paja/mhpower_zapojeni_spravne.png", dpi=130, bbox_inches="tight")
print("OK saved /home/paja/mhpower_zapojeni_spravne.png")
