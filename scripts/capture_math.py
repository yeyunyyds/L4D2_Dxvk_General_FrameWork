"""D3D9 bytecode usage and explicitly selected matrix hypotheses; no camera guessing."""
import math
import struct


def inverse(m):
    if len(m)!=16 or not all(math.isfinite(x) for x in m): raise ValueError('Matrix is not finite 4x4')
    a=[[float(m[r*4+c]) for c in range(4)]+[float(r==c) for c in range(4)] for r in range(4)]
    scale=max(abs(x) for x in m)
    for col in range(4):
        pivot=max(range(col,4),key=lambda row:abs(a[row][col]))
        if abs(a[pivot][col])<=max(scale,1)*1e-12: raise ValueError('Singular/ill-conditioned matrix')
        a[col],a[pivot]=a[pivot],a[col]
        d=a[col][col];a[col]=[x/d for x in a[col]]
        for row in range(4):
            if row!=col:
                d=a[row][col];a[row]=[x-d*y for x,y in zip(a[row],a[col])]
    return [a[r][4+c] for r in range(4) for c in range(4)]


def mul(m,v): return [sum(m[r*4+c]*v[c] for c in range(4)) for r in range(4)]
def transpose(m): return [m[c*4+r] for r in range(4) for c in range(4)]


def shader_usage(data):
    if len(data)%4 or len(data)<8: raise ValueError('Invalid shader bytecode')
    words=struct.unpack('<'+'I'*(len(data)//4),data);version=words[0]
    if (version>>16) not in (0xfffe,0xffff): raise ValueError('Not D3D9 shader bytecode')
    major=(version>>8)&255
    if major not in (2,3): return {'shader_model':major,'supported':False,'read_constants':[],'matrix_starts':[]}
    def register(token):
        t=((token>>28)&7)|((token>>8)&24)
        return token&2047 if token&0x80000000 and t==2 else None
    reads=set();defined=set();spans=set();relative=False;i=1;ended=False
    while i<len(words):
        token=words[i];op=token&65535
        if op==65535: ended=True;break
        if op==65534:
            n=(token>>16)&32767;i+=1+n
            if i>len(words): raise ValueError('Truncated shader comment')
            continue
        n=(token>>24)&15
        if i+1+n>len(words): raise ValueError('Truncated shader instruction')
        args=words[i+1:i+1+n]
        if op in (47,48,81):
            if args and op==81 and register(args[0]) is not None: defined.add(register(args[0]))
        else:
            for operand in args[1:]:
                r=register(operand)
                if r is not None: reads.add(r);relative|=bool(operand&0x2000)
            if op==20 and len(args)>=3 and register(args[-1]) is not None:
                r=register(args[-1]);spans.add(r);reads.update(range(r,r+4))
        i+=1+n
    if not ended: raise ValueError('Missing shader END')
    reads-=defined
    starts={r for r in reads if all(r+j in reads for j in range(4))}|spans
    starts={r for r in starts if not any(r+j in defined for j in range(4))}
    return {'shader_model':major,'supported':True,'read_constants':sorted(reads),'matrix_starts':sorted(starts),'relative_constant_reads':relative,'defined_constants':sorted(defined)}


def project_previous(x,y,z,current_inverse,previous_matrix,current_view,previous_view):
    if not math.isfinite(z) or current_view['max_z']==current_view['min_z']: raise ValueError('Invalid depth/viewport')
    clip=[2*(x-current_view['x'])/current_view['width']-1,
          1-2*(y-current_view['y'])/current_view['height'],
          (z-current_view['min_z'])/(current_view['max_z']-current_view['min_z']),1]
    world=mul(current_inverse,clip)
    if abs(world[3])<1e-12: raise ValueError('Invalid unprojection W')
    world=[v/world[3] for v in world]
    p=mul(previous_matrix,world)
    if p[3]<=1e-12: raise ValueError('Point behind previous camera')
    p=[v/p[3] for v in p]
    px=previous_view['x']+(p[0]+1)*previous_view['width']/2
    py=previous_view['y']+(1-p[1])*previous_view['height']/2
    depth=previous_view['min_z']+p[2]*(previous_view['max_z']-previous_view['min_z'])
    return px,py,depth
