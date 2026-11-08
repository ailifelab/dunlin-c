"""Standalone external C/C++ declarations and link smoke, no private headers."""
import pathlib,subprocess,tempfile,sys
root=pathlib.Path(__file__).resolve().parent.parent
build=pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='dunlin-public-api-') as tmp:
 p=pathlib.Path(tmp)
 for header in sorted((root/'include/dunlin/compact').glob('*.h')):
  for compiler,lang in [('cc','c'),('c++','c++')]:
   subprocess.run([compiler,'-x',lang,'-std=c11' if lang=='c' else '-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'include'),'-fsyntax-only','-'],input=('#include <dunlin/compact/%s>\n'%header.name).encode(),check=True)
 source=p/'external.c';source.write_text('''#include <dunlin/compact/runtime.h>
#include <dunlin/compact/http.h>
#include <dunlin/compact/tools.h>
#include <dunlin/compact/session.h>
#include <dunlin/compact/json.h>
#include <dunlin/compact/png.h>
#include <stdlib.h>
#include <assert.h>
static void *allocate(void *u,size_t n){(void)u;return malloc(n);}
static void release(void *u,void *p){(void)u;free(p);}
int main(void) {
 ak_platform host={0};host.alloc=allocate;host.free=release;
 ak_context *c=ak_create(&host);assert(c && ak_root(c));
 assert(ah_default_limits().max_rounds>0);
 ah_png image={0};assert(!ah_png_content("",&image));
 assert(dunlin_model_url_valid("http://127.0.0.1:1234/v1"));
 assert(ak_destroy(c)==AK_OK);return 0;
}
''')
 extra=['/opt/homebrew/opt/json-c/lib/libjson-c.a','-lcurl','-lm'] if sys.platform=='darwin' else subprocess.check_output(['pkg-config','--libs','json-c','libcurl']).decode().split()+['-lm']
 subprocess.run(['cc','-std=c11','-Werror=implicit-function-declaration','-I'+str(root/'include'),str(source),str(build/'libdunlin.a'),str(build/'libdunlin-kernel.a')]+extra+['-o',str(p/'host')],check=True)
 subprocess.run([str(p/'host')],check=True)
print('PASS all public headers standalone C11/C++11 and external linked C host')
