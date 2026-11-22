#!/usr/bin/env python3
"""dungeon_map.py <dungeon.dat> [level]: print one level of a CSBwin-format dungeon as text, with
its doors, wall texts and actuators (type 127 = a champion mirror). Used to find the way from the
start to the first mirror for the emulator party test (see README.md here). Reads the CSBwin
"Extended Features" header, then the classic big-endian dungeon header behind it."""
import struct,sys,os
d=open(sys.argv[1],'rb').read()
assert d[:19]==b' Extended Features ', 'not a CSBwin extended dungeon file'
LM=struct.unpack('<i',d[20:24])[0]; _flags=d[37]; _gis=struct.unpack('<I',d[44:48])[0]
o=512+3*LM+_gis
if _flags&1:
    while True:
        _t=d[o:o+3]; o+=3
        if _t[0]>63: break
ENDI=os.environ.get('ENDI','<')
seed,mapsz=struct.unpack('>HH',d[o:o+4]); nm=d[o+4]
txt,start,objl=struct.unpack('>HHH',d[o+6:o+12]); cnt=struct.unpack('>16H',d[o+12:o+44])
p=o+44
maps=[]
for i in range(nm):
    off,_,ox,oy,a,b,c,e=struct.unpack('>HIBBHHHH',d[p:p+16]); p+=16
    maps.append(dict(off=off,ox=ox,oy=oy,level=a&63,w=((a>>6)&31)+1,h=((a>>11)&31)+1))
tot=sum(m['w'] for m in maps)
cols=struct.unpack('>%dH'%tot,d[p:p+2*tot]); p+=2*tot
objs=struct.unpack('>%dH'%objl,d[p:p+2*objl]); p+=2*objl
p+=4*txt
ctl,=struct.unpack('<I',d[p:p+4]); p+=4; p+=2*ctl
sizes=[4,6,4,10]
dbo=[]
for t in range(4):
    dbo.append(p); p+=sizes[t]*cnt[t]
mapdata=len(d)-2-mapsz
full=[4,6,4,10,16,4,4,4,4,8,4,0,0,0,8,4]
print('fwd end',dbo[0]+sum(full[i]*cnt[i] for i in range(16)),'cnt',cnt)
print('maps',maps[0],'start x',start&31,'y',(start>>5)&31,'dir',(start>>10)&3,'ctl',ctl,'mapdata',mapdata,'p after actuators',p)
tmap=d[512:512+LM]
imap=struct.unpack(ENDI+'%dH'%LM,d[512+LM:512+3*LM])
def sq(L,x,y): m=maps[L]; return d[mapdata+m['off']+x*m['h']+y]
def things(L,x,y):
    m=maps[L]; col=sum(mm['w'] for mm in maps[:L])+x
    idx=cols[col]
    for yy in range(y):
        if sq(L,x,yy)&0x10: idx+=1
    if not sq(L,x,y)&0x10: return []
    out=[]; t=objs[idx]; n=0
    while t not in (0xFFFE,0xFFFF) and n<20:
        n+=1
        pos=t>>14; nn=t&0x3fff
        if nn>=LM: out.append((99,pos,nn,None,'')); break
        ty=tmap[nn]; ix=imap[nn]
        if ty<4:
            rec=d[dbo[ty]+ix*sizes[ty]:dbo[ty]+(ix+1)*sizes[ty]]
            nxt,w1=struct.unpack('>HH',rec[:4])
            out.append((ty,pos,ix,w1,rec.hex()))
            t=nxt
        else:
            out.append((ty,pos,ix,None,'')); break
    return out
L=int(sys.argv[2]) if len(sys.argv)>2 else 0
m=maps[L]
ch='#.PSDT~?'
for y in range(m['h']):
    print(''.join(ch[sq(L,x,y)>>5] for x in range(m['w'])), y)
for y in range(m['h']):
    for x in range(m['w']):
        for (ty,pos,ix,w1,h) in things(L,x,y):
            if ty==3: print('act at',x,y,'side',pos,'type',w1&127,'data',w1>>7,h)
            elif ty==0: print('door at',x,y,h)
            elif ty==2: print('text at',x,y,'side',pos,h)
