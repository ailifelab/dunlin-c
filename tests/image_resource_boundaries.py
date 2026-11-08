#!/usr/bin/env python3
"""Independent PNG/base64 oracle and real SQLite lock/transaction negatives."""
import base64,ctypes as C,ctypes.util,hashlib,pathlib,random,sqlite3,struct,sys,tempfile,zlib

lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
P=C.c_void_p;S=C.c_char_p;I=C.c_int
class Info(C.Structure):
    _fields_=[('sha256',C.c_char*65),('width',C.c_uint),('height',C.c_uint),('bytes',C.c_size_t)]
class Draft(C.Structure):
    _fields_=[('id',C.c_char*33),('name',C.c_char*257),('kind',C.c_char*16),('image',Info)]
for name,args,result in [('dl_png_inspect',[P,C.c_size_t,C.POINTER(Info)],I),
    ('dl_png_data_url',[P,C.c_size_t,C.POINTER(P)],I),('dl_image_free',[P],None),
    ('dl_images_open',[P,C.c_uint64],P),('dl_images_close',[P],None),
    ('dl_images_stage',[P,S,S,S,S,P,C.c_size_t,C.POINTER(Draft)],I),
    ('dl_images_draft',[P,S,C.POINTER(Draft)],I),('dl_images_remove_draft',[P,S,S],I),
    ('dl_images_bind',[P,S,S,C.c_int64,S],I),('dl_images_delete_owner',[P,S],I),
    ('dl_images_read',[P,S,C.c_int64,S,S,C.POINTER(P),C.POINTER(C.c_size_t),C.POINTER(Draft)],I)]:
    f=getattr(lib,name);f.argtypes=args;f.restype=result
def chunk(kind,p=b''):
    return struct.pack('>I',len(p))+kind+p+struct.pack('>I',zlib.crc32(kind+p)&0xffffffff)
signature=b'\x89PNG\r\n\x1a\n'
def header(w=1,h=1,depth=8,color=6,interlace=0,compression=0,filter_method=0):
    return chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,depth,color,compression,filter_method,interlace))
def png(raw=b'\0\xff\x00\x00\xff',**kw):
    return signature+header(**kw)+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND')
def inspect(data,expected=0):
    info=Info();C.memset(C.byref(info),0x7f,C.sizeof(info))
    result=lib.dl_png_inspect(data,len(data),C.byref(info))
    assert result==expected,(result,expected,len(data),data[:40])
    if not result:assert info.bytes==len(data) and info.sha256.decode()==hashlib.sha256(data).hexdigest()
    else:assert not info.bytes and not info.width and not info.height and not info.sha256
    return info
red=png();green=png(b'\0\x00\xff\x00\xff')
for color,depths,channels in [(0,[1,2,4,8,16],1),(2,[8,16],3),(3,[1,2,4,8],1),(4,[8,16],2),(6,[8,16],4)]:
    for depth in depths:
        rows=b'\0'+bytes((channels*depth+7)//8)
        palette=chunk(b'PLTE',b'\0\0\0') if color==3 else b''
        data=signature+header(depth=depth,color=color)+palette+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND')
        info=inspect(data);assert info.width==1 and info.height==1
        out=P();assert lib.dl_png_data_url(data,len(data),C.byref(out))==0 and out
        try:
            url=C.string_at(out).decode('ascii');assert url=='data:image/png;base64,'+base64.b64encode(data).decode()
            assert base64.b64decode(url.split(',',1)[1],validate=True)==data
        finally:lib.dl_image_free(out)
for dims in [(1024,1024),(1024,1),(1,1024)]:
    w,h=dims;raw=(b'\0'+bytes(w*8))*h
    info=inspect(png(raw,w=w,h=h,depth=16));assert (info.width,info.height)==dims
for w,h in [(9,9),(8,8),(1,1),(1,9),(9,1)]:
    raw=b''
    for x,y,dx,dy in [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]:
        width=max(0,(w-x+dx-1)//dx);height=max(0,(h-y+dy-1)//dy)
        if width and height:raw+=(b'\0'+bytes(width*4))*height
    inspect(png(raw,w=w,h=h,interlace=1))
max_png=red[:-12]+chunk(b'vpAg',bytes(1048576-len(red)-12))+red[-12:]
assert len(max_png)==1048576;inspect(max_png);inspect(max_png+b'\0',2)
bad=[b'',b'JPEG',red[1:],red[:-1],red+b'extra',signature+chunk(b'IEND'),
     signature+header()+header()+chunk(b'IDAT',zlib.compress(b'\0\0\0\0\0'))+chunk(b'IEND'),
     signature+header(color=3)+chunk(b'IDAT',zlib.compress(b'\0\0'))+chunk(b'IEND'),
     signature+header(color=0)+chunk(b'PLTE',b'\0\0\0')+chunk(b'IDAT',zlib.compress(b'\0\0'))+chunk(b'IEND'),
     signature+header()+chunk(b'IDAT',zlib.compress(b'\0\0\0\0\0'))+chunk(b'PLTE',b'\0\0\0')+chunk(b'IEND'),
     signature+header()+chunk(b'IDAT',b'not zlib')+chunk(b'IEND'),
     signature+header()+chunk(b'IDAT',zlib.compress(b'\0\0\0\0\0')+b'extra')+chunk(b'IEND'),
     signature+header()+chunk(b'IDAT',zlib.compress(bytes(100000)))+chunk(b'IEND'),
     signature+header()+chunk(b'IDAT',zlib.compress(b'\0\0\0\0\0'))+chunk(b'IDaT')+chunk(b'IEND'),
     png(b'\0\0\0\0\0',depth=4),png(b'\0\0\0\0\0',color=1),png(compression=1),png(filter_method=1),png(interlace=2)]
stream=zlib.compress(b'\0\0\0\0\0')
bad.append(signature+header()+chunk(b'IDAT',stream[:3])+chunk(b'tEXt',b'k\0v')+chunk(b'IDAT',stream[3:])+chunk(b'IEND'))
dictionary=zlib.compressobj(zdict=b'owned dictionary')
bad.append(signature+header()+chunk(b'IDAT',dictionary.compress(b'\0\0\0\0\0')+dictionary.flush())+chunk(b'IEND'))
for data in bad:inspect(data,1)
for w,h in [(0,1),(1,0),(1025,1),(1,1025),(0xffffffff,1)]:inspect(png(w=w,h=h),2)
for n in range(len(red)):inspect(red[:n],1)
rng=random.Random(0xD0111C)
for _ in range(12000):
    data=bytearray(red if rng.randrange(2) else bytes(rng.randrange(256) for _ in range(rng.randrange(300))))
    if data:
        for _ in range(1+rng.randrange(3)):data[rng.randrange(len(data))]^=1<<rng.randrange(8)
    info=Info();result=lib.dl_png_inspect(bytes(data),len(data),C.byref(info));assert result in (0,1,2)
    if not result:assert info.sha256.decode()==hashlib.sha256(data).hexdigest()
print('PASS independent hashlib/base64 oracle, all PNG color/depth layouts, maximum encoded and decoded bounds, Adam7 passes, malformed streams and deterministic parser stress',flush=True)

sql=C.CDLL('/usr/lib/libsqlite3.dylib' if sys.platform=='darwin' else ctypes.util.find_library('sqlite3'))
for name,args,result in [('sqlite3_open',[S,C.POINTER(P)],I),('sqlite3_close',[P],I),
    ('sqlite3_exec',[P,S,P,P,P],I),('sqlite3_busy_timeout',[P,I],I)]:
    f=getattr(sql,name);f.argtypes=args;f.restype=result
with tempfile.TemporaryDirectory(prefix='dunlin-image-resource-') as tmp:
    path=pathlib.Path(tmp)/'resources.sqlite';db=P();assert sql.sqlite3_open(str(path).encode(),C.byref(db))==0
    store=lib.dl_images_open(db,3*1048576);assert store
    def stage(owner,data,expected=None):
        out=Draft();status=lib.dl_images_stage(store,owner,expected,b'owned.png',b'file',data,len(data),C.byref(out));return status,out
    status,first=stage(b'chat',red);assert status==0
    other=sqlite3.connect(path,isolation_level=None);other.execute('BEGIN IMMEDIATE')
    assert sql.sqlite3_busy_timeout(db,1)==0
    status,out=stage(b'chat',green,first.id);assert status==5 and not out.id
    other.execute('ROLLBACK')
    current=Draft();assert lib.dl_images_draft(store,b'chat',C.byref(current))==0 and current.id==first.id
    # A foreign owner cannot consume/delete this binding, even with its known ID.
    assert lib.dl_images_remove_draft(store,b'foreign',first.id)==4
    assert sql.sqlite3_exec(db,b'BEGIN IMMEDIATE',None,None,None)==0
    assert lib.dl_images_bind(store,b'foreign',first.id,0,hashlib.sha256(b'source').hexdigest().encode())==4
    assert sql.sqlite3_exec(db,b'ROLLBACK',None,None,None)==0
    other.execute('PRAGMA ignore_check_constraints=ON') # deliberate corrupt-store fixture
    for mutation in ['UPDATE image_resources SET width=4294967297',"UPDATE image_drafts SET name=CAST(x'610062' AS TEXT)","UPDATE image_drafts SET sha='missing'"]:
        other.execute('BEGIN IMMEDIATE');other.execute(mutation);other.execute('COMMIT')
        out=Draft();assert lib.dl_images_draft(store,b'chat',C.byref(out))==5 and not out.id
        if 'width' in mutation:other.execute('UPDATE image_resources SET width=1')
        elif 'name' in mutation:other.execute("UPDATE image_drafts SET name='owned.png'")
        else:other.execute('UPDATE image_drafts SET sha=?',(hashlib.sha256(red).hexdigest(),))
    other.close();lib.dl_images_close(store);assert sql.sqlite3_close(db)==0
print('PASS actual competing SQLite writer, rejected-write draft preservation, foreign-owner rejection and malformed/missing stored resource metadata',flush=True)
