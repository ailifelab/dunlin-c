"""Exercise the production C tar parser with adversarial actual tar bytes."""
import ctypes as C,pathlib,tempfile,tarfile,io,os,sys
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));fn=lib.dc_npm_extract;fn.argtypes=[C.c_int,C.c_char_p,C.c_size_t];fn.restype=C.c_int

def tar(name='package/main.js',kind=tarfile.REGTYPE,data=b'export {}',link=''):
 out=io.BytesIO()
 with tarfile.open(fileobj=out,mode='w',format=tarfile.USTAR_FORMAT) as f:
  i=tarfile.TarInfo(name);i.type=kind;i.linkname=link;i.size=len(data) if kind==tarfile.REGTYPE else 0;f.addfile(i,io.BytesIO(data) if i.size else None)
 return out.getvalue()
with tempfile.TemporaryDirectory(prefix='dunlin-tar-') as tmp:
 root=pathlib.Path(tmp)
 for i,(raw,expected) in enumerate([(tar(),True),(tar('package/../../outside'),False),(tar('/outside'),False),(tar('package/x',tarfile.SYMTYPE,link='/tmp/outside'),False),(tar('package/x',tarfile.LNKTYPE,link='package/main.js'),False),(tar('package/dunlin.json'),False),(tar('package/a\\b'),False),(tar('package/main.js')[:515],False),(b'x'*512,False),(tar(data=b'x'*(16*1024*1024+1)),False)]):
  folder=root/str(i);folder.mkdir();fd=os.open(folder,os.O_RDONLY|os.O_DIRECTORY)
  try:assert bool(fn(fd,raw,len(raw)))==expected,(i,expected)
  finally:os.close(fd)
 assert not (root/'outside').exists()
 print('PASS real C tar ordinary content, traversal/absolute/link/manifest overwrite/checksum/truncation/oversize rejection')
