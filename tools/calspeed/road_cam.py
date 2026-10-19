# Renders side views (both ways, across the road) from points ON the swept road behind a point-to-point start, as the
# game draws them (back faces culled, filtered). Behind the cut the road bends and drops, so start-frame cameras miss it.
#   T=<track> D=<ft back along the road, comma separated> [W=2000 H=400] python tools/calspeed/road_cam.py
# Needs tmp/scenery/t<T>/track.glb from tools/calspeed/scenery.py. Writes tmp/scenery/road<T>_<D>a.png and ...b.png.
import sys, os, math, numpy as np, subprocess
sys.argv=['x']; sys.path.insert(0,'tools/calspeed'); sys.path.insert(0,'tools'); sys.path.insert(0,'tools/trackview')
import scenery as sc, glb_view as gv
from cs import CalSpeed
c=CalSpeed(); t=int(os.environ.get('T','11'))
s0,(dx,dz)=sc.start_frame(c.asset(110+t)); nx,nz=-dz,dx
glb='tmp/scenery/t%d/track.glb'%t
gltf,b=gv.load(glb)
P,U,C,T=gv.triangles(gltf,b,None); P=P.copy(); P[:,:,0]*=-1
cen=P.mean(1)
a=(cen[:,0]-s0[0])*dx+(cen[:,2]-s0[2])*dz; l=(cen[:,0]-s0[0])*nx+(cen[:,2]-s0[2])*nz
k=np.argmin(np.abs(a)+np.abs(l)+np.where(np.abs(cen[:,1]-s0[1])<3,0,1e6)); road=T[k]
R=cen[(T==road)&(a<-20)]
# order road centroids by a walk from the cut
pts=[np.array([s0[0],s0[1],s0[2]])]; left=list(R)
path=[pts[0]]
while left:
    d=[np.linalg.norm((q-path[-1])[[0,2]]) for q in left]
    i=int(np.argmin(d))
    if d[i]>80: break
    path.append(left.pop(i))
path=np.array(path); seg=np.linalg.norm(np.diff(path[:,[0,2]],axis=0),axis=1); cum=np.concatenate([[0],np.cumsum(seg)])
W=int(os.environ.get('W','2000')); H=int(os.environ.get('H','400'))
m=lambda p:[str(-p[0]),str(p[1]),str(p[2])]
for D in [float(x) for x in os.environ['D'].split(',')]:
    i=min(int(np.searchsorted(cum,D)),len(path)-2)
    p=path[i]; q=path[min(i+5,len(path)-1)]; d=(q-p); d[1]=0; d/=np.linalg.norm(d)
    side=np.array([-d[2],0,d[0]])
    eye=p+np.array([0,6,0])
    for nm,sv in (('a',1),('b',-1)):
        at=eye+side*sv*1000+np.array([0,10,0])
        out='tmp/scenery/road%d_%d%s.png'%(t,int(D),nm)
        subprocess.run([sys.executable,'tools/glb_view.py',glb,out,'--eye',*m(eye),'--at',*m(at),'--fov','100','--width',str(W),'--height',str(H),'--cull','1','--bilinear'],check=True,capture_output=True)
        print(out, 'at along %.0f across %.0f h %.0f'%(np.dot((p-s0)[[0,2]],[dx,dz]),np.dot((p-s0)[[0,2]],[nx,nz]),p[1]-s0[1]))
