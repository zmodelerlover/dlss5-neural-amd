import struct, sys, math
P='D:/pcsx2-v2.8.2-test/dlssnr_on_amd_weights.bin'
d=open(P,'rb').read()
n,data_base=struct.unpack_from('<II',d,8); off=16; ents={}
for i in range(n):
    ln=d[off]; off+=1; nm=d[off:off+ln].decode(); off+=ln
    o,s=struct.unpack_from('<QQ',d,off); off+=16; ents[nm]=(o,s)
o,s=ents['block0.layer0.layer']; base=data_base+o
R=8208; STRIDE=64                      # v_lshlrev_b32 v6, 6, v6  -> row*64

# lane -> byte offset within the row, derived from the ISA operand pairing
# (v23=s[4:5]+2 b32 -> +2,+4 ; v24=s[4:5]+6 -> +6 ; v16/17=+16 ; v18/19=+32 ; v20/21=+48)
LANE={0:0, 1:2, 8:4, 9:6, 2:16, 3:18, 10:20, 11:22,
      4:32, 5:34, 12:36, 13:38, 6:48, 7:50, 14:52, 15:54}
SEM={0:'noise0',1:'noise1',2:'noise2',3:'BIAS 1.0',4:'imgA.r',5:'imgA.g',6:'imgA.b',
     7:'imgB.r',8:'imgB.g',9:'imgB.b',10:'PreParams+48  (host writes 0)',
     11:'ctl0 +40',12:'ctl1 +44',13:'ctl2 +72',14:'ctl3 +76',15:'HARD ZERO in kernel'}

def f16(i): return struct.unpack_from('<e',d,i)[0]

NROWS=int(sys.argv[1]) if len(sys.argv)>1 else 32
print("block0.layer0.layer size=%d  proj@+%d stride=%d rows=%d  (ends at +%d)"
      % (s,R,STRIDE,NROWS,R+NROWS*STRIDE))
print()
print("lane  semantic                        sum|w|      max|w|     rms      nonzero/%d" % NROWS)
res={}
for L in range(16):
    ws=[f16(base+R+r*STRIDE+LANE[L]) for r in range(NROWS)]
    ws=[0.0 if (w!=w) else w for w in ws]
    a=[abs(w) for w in ws]
    rms=math.sqrt(sum(w*w for w in ws)/len(ws))
    nz=sum(1 for w in a if w>0)
    res[L]=(sum(a),max(a),rms,nz,ws)
    print("  %2d  %-30s %8.4f  %8.4f  %8.4f   %d" % (L,SEM[L],sum(a),max(a),rms,nz))

print()
print("VERDICT INPUTS")
ctl=[res[L][2] for L in (11,12,13,14)]
print("  lane10 rms            = %.6f" % res[10][2])
print("  shipped ctl rms       = %s  (mean %.6f)" % (["%.6f"%c for c in ctl], sum(ctl)/4))
print("  lane15 rms (dead ctl) = %.6f" % res[15][2])
print("  lane3  rms (bias)     = %.6f" % res[3][2])
print()
print("  lane10 / mean(ctl11-14) = %.3f" % (res[10][2]/(sum(ctl)/4)))
print("  lane10 nonzero rows     = %d / %d" % (res[10][3], NROWS))
print("  lane15 nonzero rows     = %d / %d" % (res[15][3], NROWS))
print()
print("first 8 rows, lanes 10..15:")
for r in range(min(8,NROWS)):
    print("   row%-3d  l10=%9.5f  l11=%9.5f l12=%9.5f l13=%9.5f l14=%9.5f  l15=%9.5f"
          % (r,res[10][4][r],res[11][4][r],res[12][4][r],res[13][4][r],res[14][4][r],res[15][4][r]))
