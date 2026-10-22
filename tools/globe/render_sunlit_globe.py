#!/usr/bin/env python3
"""Mock-up renderer for Ragtime's sunlit globe (numpy + PIL).

Reference for the theme, not app code: light blue oceans, near-white land,
dark blue coasts and borders, lit from the subsolar point so the greyline
shows. Reads Glissando's Natural Earth tables (LandOutlines.h, BorderLines.h).

Usage: render_sunlit_globe.py '{"out": "g.png", "size": 640, "center": [47.5, -123],
        "home": [47.5, -123], "target": [41.7, -72.7], "dots": [],
        "utc": [2026, 10, 21, 22, 36], "glissando": "/path/to/glissando",
        "admin1": "us_canada_admin1_lines.json"}'
"""
import re, math, sys, json, numpy as np
from PIL import Image, ImageDraw
cfg=json.loads(sys.argv[1])
SRC=cfg.get('glissando','../glissando').rstrip('/')+'/app/src/gui/glissando/'
def arr(txt,name):
    m=re.search(name+r'\[[^\]]*\]\s*=\s*\{(.*?)\};',txt,re.S)
    return [int(v) for v in re.findall(r'-?\d+',m.group(1))]
land=open(SRC+'LandOutlines.h').read(); bord=open(SRC+'BorderLines.h').read()
RS=arr(land,'RING_START'); LP=arr(land,'POINTS')
LS=arr(bord,'LINE_START'); BP=arr(bord,'POINTS')
def rings(starts,pts):
    out=[]
    for i in range(len(starts)-1):
        a,b=starts[i],starts[i+1]
        out.append([(pts[2*k]/100.0,pts[2*k+1]/100.0) for k in range(a,b)])
    return out
landR=rings(RS,LP); bordL=rings(LS,BP)

def subsolar(y,mo,d,h,mi):
    import datetime
    t=datetime.datetime(y,mo,d,h,mi)
    n=(t-datetime.datetime(2000,1,1,12)).total_seconds()/86400
    L=math.radians((280.460+0.9856474*n)%360); g=math.radians((357.528+0.9856003*n)%360)
    lam=L+math.radians(1.915)*math.sin(g)+math.radians(0.020)*math.sin(2*g)
    eps=math.radians(23.439-0.0000004*n)
    dec=math.asin(math.sin(eps)*math.sin(lam))
    ra=math.atan2(math.cos(eps)*math.sin(lam),math.cos(lam))
    gmst=(280.46061837+360.98564736629*n)%360
    lon=(math.degrees(ra)-gmst+540)%360-180
    return math.degrees(dec),lon
slat,slon=subsolar(*cfg.get('utc',[2026,10,21,22,36]))
print('subsolar',slat,slon,file=sys.stderr)

out=cfg['out']; S=cfg.get('size',640)
lat0,lon0=cfg['center']
SS=3; N=S*SS; R=N/2-2*SS
def vec(lat,lon):
    la,lo=math.radians(lat),math.radians(lon)
    return np.array([math.cos(la)*math.cos(lo),math.cos(la)*math.sin(lo),math.sin(la)])
c=vec(lat0,lon0); east=np.array([-math.sin(math.radians(lon0)),math.cos(math.radians(lon0)),0.0])
north=np.cross(c,east)
def proj(lat,lon):
    v=vec(lat,lon); z=v@c
    return N/2+R*(v@east), N/2-R*(v@north), z
W,H=3600,1800
eq=Image.new('L',(W,H),0); de=ImageDraw.Draw(eq)
for r in landR:
    de.polygon([((lo+180)*10,(90-la)*10) for lo,la in r],fill=255)
landmask=np.array(eq)>127
yy,xx=np.mgrid[0:N,0:N].astype(np.float64)
u=(xx-N/2)/R; v=(N/2-yy)/R; rr=u*u+v*v; inside=rr<=1
w=np.sqrt(np.clip(1-rr,0,1))
P=(u[...,None]*east+v[...,None]*north+w[...,None]*c)
lat=np.degrees(np.arcsin(np.clip(P[...,2],-1,1))); lon=np.degrees(np.arctan2(P[...,1],P[...,0]))
ix=np.clip(((lon+180)*10).astype(int),0,W-1); iy=np.clip(((90-lat)*10).astype(int),0,H-1)
isl=landmask[iy,ix]&inside
ocean=np.array([168,212,242],float); landc=np.array([247,245,238],float)
img=np.where(isl[...,None],landc,ocean)
s=vec(slat,slon); cosz=P@s
def smooth(e0,e1,x):
    t=np.clip((x-e0)/(e1-e0),0,1); return t*t*(3-2*t)
day=smooth(math.sin(math.radians(-12)),math.sin(math.radians(3)),cosz)
night=np.array([28,44,86],float)
lit=img*(0.93+0.07*np.clip(cosz,0,1))[...,None]
dark=img*0.30+night*0.55
img=dark*(1-day[...,None])+lit*day[...,None]
img=img*(0.88+0.12*w)[...,None]
rgba=np.zeros((N,N,4),np.uint8); rgba[...,:3]=np.clip(img,0,255); rgba[...,3]=np.where(inside,255,0)
im=Image.fromarray(rgba,'RGBA'); d=ImageDraw.Draw(im,'RGBA')
def polyline(pts,col,width):
    prev=None
    for lo,la in pts:
        x,y,z=proj(la,lo)
        if prev and z>0 and prev[2]>0: d.line([prev[:2],(x,y)],fill=col,width=width)
        prev=(x,y,z)
for la in range(-60,90,30): polyline([(lo,la) for lo in np.arange(-180,181,2)],(60,100,160,45),SS)
for lo in range(-180,180,30): polyline([(lo,la) for la in np.arange(-90,91,2)],(60,100,160,45),SS)
for r in landR: polyline(r+[r[0]],(20,48,104,255),int(1.6*SS))
for l in bordL: polyline(l,(20,48,104,220),int(1.1*SS))
if cfg.get('admin1'):
    for l in json.load(open(cfg['admin1'])): polyline(l,(20,48,104,120),int(0.8*SS))
def gc(p,q,n=200):
    a=vec(*p); b=vec(*q); om=math.acos(float(np.clip(a@b,-1,1))); pts=[]
    for i in range(n+1):
        t=i/n; vv=(math.sin((1-t)*om)*a+math.sin(t*om)*b)/math.sin(om)
        pts.append((math.degrees(math.atan2(vv[1],vv[0])),math.degrees(math.asin(vv[2]))))
    return pts
home=cfg['home']
if cfg.get('target'): polyline(gc(home,cfg['target']),(200,40,30,255),int(2.2*SS))
def dot(p,col,r):
    x,y,z=proj(*p)
    if z>0: d.ellipse([x-r*SS,y-r*SS,x+r*SS,y+r*SS],fill=col,outline=(255,255,255,255),width=SS)
for p in cfg.get('dots',[]): dot(p,(200,40,30,255),4)
if cfg.get('target'): dot(cfg['target'],(200,40,30,255),6)
dot(home,(20,48,104,255),6)
im=im.resize((S,S),Image.LANCZOS); im.save(out)
