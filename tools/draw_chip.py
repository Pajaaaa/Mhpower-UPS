import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyBboxPatch, Circle, Wedge
from matplotlib.lines import Line2D

fig, ax = plt.subplots(figsize=(15, 10), dpi=130)
ax.set_xlim(0, 15); ax.set_ylim(0, 10); ax.axis("off")
BLACK="#1a1a1a"; RED="#c0392b"; GREEN="#1a7a3a"; SIG="#15497a"

def wire(pts, color=BLACK, lw=2.2):
    ax.add_line(Line2D([p[0] for p in pts],[p[1] for p in pts],color=color,lw=lw,
                solid_capstyle="round", solid_joinstyle="round", zorder=2))
def node(x,y,c=BLACK):
    ax.add_patch(Circle((x,y),0.055,color=c,zorder=6))
def res(x,y,label,horiz=True,L=0.95,W=0.30,color=BLACK):
    if horiz:
        ax.add_patch(Rectangle((x,y-W/2),L,W,fc="white",ec=color,lw=2.2,zorder=4))
        ax.text(x+L/2,y+W/2+0.14,label,ha="center",va="bottom",fontsize=10,zorder=5)
        return (x,y),(x+L,y)
def cap(x,y_top,y_bot,label):
    ymid=(y_top+y_bot)/2
    wire([(x,y_top),(x,ymid+0.09)],RED)
    ax.add_line(Line2D([x-0.22,x+0.22],[ymid+0.09,ymid+0.09],color=BLACK,lw=2.6,zorder=5))
    ax.add_line(Line2D([x-0.22,x+0.22],[ymid-0.09,ymid-0.09],color=BLACK,lw=2.6,zorder=5))
    wire([(x,ymid-0.09),(x,y_bot)])
    ax.text(x+0.30,ymid,label,ha="left",va="center",fontsize=10)
def gnd(x,y):
    wire([(x,y),(x,y-0.16)])
    for i,w in enumerate([0.34,0.21,0.10]):
        yy=y-0.16-i*0.11
        ax.add_line(Line2D([x-w/2,x+w/2],[yy,yy],color=BLACK,lw=2.4,zorder=5))

# ---------- title ----------
ax.text(7.5,9.7,"MHpower – zapojení 74LVC14A (5 V → 3,3 V level shifter)",
        ha="center",va="top",fontsize=16,fontweight="bold")
ax.text(7.5,9.28,"Použita 2 hradla ze 6.  Signály vyjdou invertované – firmware si edge i invert najde sám.",
        ha="center",va="top",fontsize=10.5,color="#555")

# ================= CHIP (DIP-14 top view) =================
cx0,cx1,cy0,cy1 = 6.1, 8.4, 1.9, 7.5
bodyfc="#2b2b2b"
ax.add_patch(FancyBboxPatch((cx0,cy0),cx1-cx0,cy1-cy0,
        boxstyle="round,pad=0.0,rounding_size=0.12",fc=bodyfc,ec="black",lw=2,zorder=3))
# notch at top center
ax.add_patch(Wedge((( cx0+cx1)/2, cy1),0.22,180,360,fc="white",ec="black",lw=1.5,zorder=4))
# pin-1 dot
ax.add_patch(Circle((cx0+0.33,cy1-0.33),0.10,fc="#dddddd",ec="none",zorder=5))
ax.text((cx0+cx1)/2, cy1-0.28,"74LVC14A",ha="center",va="top",color="white",
        fontsize=12.5,fontweight="bold",zorder=5)
ax.text((cx0+cx1)/2, cy1-0.74,"SN74LVC14A",ha="center",va="top",color="#bbb",
        fontsize=8.5,zorder=5)

pin_y = [6.85, 6.05, 5.25, 4.45, 3.65, 2.85, 2.05]  # 7 rows
left_pins  = [(1,"1A"),(2,"1Y"),(3,"2A"),(4,"2Y"),(5,"3A"),(6,"3Y"),(7,"GND")]
right_pins = [(14,"VCC"),(13,"6A"),(12,"6Y"),(11,"5A"),(10,"5Y"),(9,"4A"),(8,"4Y")]
Lconn=cx0-0.30; Rconn=cx1+0.30
for (num,fn),y in zip(left_pins,pin_y):
    ax.add_patch(Rectangle((cx0-0.30,y-0.09),0.30,0.18,fc="#cfcfcf",ec="black",lw=1.2,zorder=4))
    ax.text(cx0-0.15,y,str(num),ha="center",va="center",fontsize=9,fontweight="bold",zorder=5)
    ax.text(cx0+0.12,y,fn,ha="left",va="center",color="white",fontsize=10,zorder=5)
for (num,fn),y in zip(right_pins,pin_y):
    ax.add_patch(Rectangle((cx1,y-0.09),0.30,0.18,fc="#cfcfcf",ec="black",lw=1.2,zorder=4))
    ax.text(cx1+0.15,y,str(num),ha="center",va="center",fontsize=9,fontweight="bold",zorder=5)
    ax.text(cx1-0.12,y,fn,ha="right",va="center",color="white",fontsize=10,zorder=5)

yp = {i+1:pin_y[i] for i in range(7)}          # pins 1..7 -> y
yp.update({14:pin_y[0],13:pin_y[1],12:pin_y[2],11:pin_y[3],10:pin_y[4],9:pin_y[5],8:pin_y[6]})

# ================= TM1640 =================
ax.add_patch(FancyBboxPatch((0.3,3.0),2.0,4.0,boxstyle="round,pad=0.02,rounding_size=0.1",
             fc="#eef0f2",ec="black",lw=2))
ax.text(1.3,6.7,"Displej\nTM1640",ha="center",va="top",fontsize=12,fontweight="bold")
CLK,DIN,V5,GND_d = 6.0,5.2,4.4,3.6
for lbl,y in [("CLK",CLK),("DIN",DIN),("VCC 5 V",V5),("GND",GND_d)]:
    ax.text(2.18,y,lbl,ha="right",va="center",fontsize=10.5,fontweight="bold")
    node(2.3,y)

# ================= ESP32 =================
ax.add_patch(FancyBboxPatch((11.6,3.0),2.2,4.0,boxstyle="round,pad=0.02,rounding_size=0.1",
             fc="#eef0f2",ec="black",lw=2))
ax.text(12.7,6.7,"ESP32",ha="center",va="top",fontsize=12,fontweight="bold")
E3V3,G18,G23,EGND = 6.3,5.5,4.6,3.6
for lbl,y in [("3V3",E3V3),("GPIO18",G18),("GPIO23",G23),("GND",EGND)]:
    ax.text(11.78,y,lbl,ha="left",va="center",fontsize=10.5,fontweight="bold")
    node(11.6,y)

# ================= WIRES =================
# CLK -> 1k -> pin1
a,b=res(2.9,CLK,"1 kΩ")
wire([(2.3,CLK),(2.9,CLK)]); wire([(b[0],CLK),(3.95,CLK),(3.95,yp[1]),(Lconn,yp[1])])
# pin2 (1Y) -> over top -> 100R -> GPIO18
wire([(Lconn,yp[2]),(5.55,yp[2]),(5.55,8.55),(10.55,8.55),(10.55,G18)])
a,b=res(10.6,G18,"100 Ω"); wire([(10.55,G18),(10.6,G18)]); wire([(b[0],G18),(11.6,G18)])
# DIN -> 1k -> pin3
a,b=res(2.9,DIN,"1 kΩ")
wire([(2.3,DIN),(2.9,DIN)]); wire([(b[0],DIN),(3.95,DIN),(3.95,yp[3]),(Lconn,yp[3])])
# pin4 (2Y) -> under bottom -> 100R -> GPIO23
wire([(Lconn,yp[4]),(5.2,yp[4]),(5.2,1.15),(10.15,1.15),(10.15,G23)])
a,b=res(10.2,G23,"100 Ω"); wire([(10.15,G23),(10.2,G23)]); wire([(b[0],G23),(11.6,G23)])

# VCC: pin14 -> 3V3 (red)
wire([(Rconn,yp[14]),(8.9,yp[14]),(8.9,8.05),(11.0,8.05),(11.0,E3V3),(11.6,E3V3)],RED)
node(8.9,8.05,RED)
ax.text(9.9,8.25,"3,3 V",ha="center",fontsize=11,color=RED,fontweight="bold")
# 100nF decoupling near chip
wire([(8.9,8.05),(9.55,8.05)],RED); 
cap(9.55,8.05,7.05,"100 nF"); gnd(9.55,7.05)

# GND rail
RAIL=0.70
wire([(2.3,GND_d),(1.05,GND_d),(1.05,RAIL),(12.95,RAIL)])           # TM1640 GND + rail
wire([(11.6,EGND),(12.95,EGND),(12.95,RAIL)])                       # ESP GND
node(12.95,RAIL)
wire([(Lconn,yp[7]),(4.55,yp[7]),(4.55,RAIL)]); node(4.55,RAIL)     # chip pin7 GND
gnd(7.0,RAIL)

# Unused inputs -> GND (local, green)
# pin5 (left)
wire([(Lconn,yp[5]),(5.45,yp[5])],GREEN); gnd(5.45,yp[5])
# pin9,11,13 (right)
for p in [9,11,13]:
    wire([(Rconn,yp[p]),(9.05,yp[p])],GREEN); gnd(9.05,yp[p])

# ---------- notes box (top-left, open space) ----------
ax.add_patch(FancyBboxPatch((0.25,7.28),4.55,1.85,boxstyle="round,pad=0.04,rounding_size=0.08",
             fc="#fffdf2",ec="#c9b27a",lw=1.6))
notes=[
 ("Poznámky:", BLACK, True),
 ("• Použita jen 2 hradla ze 6.", BLACK, False),
 ("• Nepoužité VSTUPY 5, 9, 11, 13 → GND", GREEN, False),
 ("• Nepoužité výstupy 6, 8, 10, 12 = nezapojeno", GREEN, False),
 ("• Společná zem: displej + ESP32 + pin 7", BLACK, False),
 ("• 100 nF co nejblíž pinu 14.", RED, False),
]
for i,(t,c,b) in enumerate(notes):
    ax.text(0.45,8.95-i*0.275,t,ha="left",va="top",fontsize=10.5 if b else 10,
            color=c,fontweight="bold" if b else "normal")

plt.savefig("/home/paja/mhpower_zapojeni_chip.png",dpi=130,bbox_inches="tight")
print("OK")
