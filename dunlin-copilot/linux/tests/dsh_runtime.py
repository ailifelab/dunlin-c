#!/usr/bin/env python3
"""Check Linux DSH activation using official release files and an isolated data directory."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import shutil
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('library')
    ap.add_argument('assets', type=Path)
    ap.add_argument('node', type=Path)
    ap.add_argument('--libc-deb', type=Path, help='Pinned Ubuntu package on glibc 2.27')
    args = ap.parse_args()
    lib = C.CDLL(args.library)
    p, s = C.c_void_p, C.c_char_p
    for name, params, result in [
            ('dc_dsh_detect', [s, s, s, p, C.POINTER(p), C.POINTER(p)], C.c_int),
            ('dc_dsh_prepare', [s, s, s, C.c_int, p, C.POINTER(p), C.POINTER(p)], p),
            ('dc_worker_call', [p, s, s, p], p), ('dc_worker_close', [p], None), ('dc_free', [p], None)]:
        f = getattr(lib, name)
        f.argtypes, f.restype = params, result

    def take(value):
        if not value:
            return None
        try:
            return C.string_at(value).decode()
        finally:
            lib.dc_free(value)

    node = args.node.resolve()
    with tempfile.TemporaryDirectory(prefix="dunlin runtime's-") as tmp:
        home = Path(tmp)
        inputs = [os.fsencode(tmp), os.fsencode(str(args.assets.resolve())), os.fsencode(str(node))]
        def open_engine(probe=False):
            error, actual = p(), p()
            result = (lib.dc_dsh_detect(*inputs, None, C.byref(error), C.byref(actual)) if probe else
                      lib.dc_dsh_prepare(*inputs, 0, None, C.byref(error), C.byref(actual)))
            return result, take(error), take(actual)

        compat = home / 'Compatibility' / 'dsh'
        available, error, _ = open_engine(True)
        if args.libc_deb:
            assert not available and error == 'engine_download_required', (available, error)
            assert not list(compat.glob('glibc-*'))
            arch = 'arm64' if os.uname().machine == 'aarch64' else 'amd64'
            cache = compat / ('libc6_2.39-0ubuntu8.12_' + arch + '.deb')
            cache.write_bytes(b'corrupt cached package')
            worker, error, _ = open_engine()
            assert not worker and error == 'engine_private_runtime_digest_failed', error
            assert not list(compat.glob('.runtime-*')) and not list(compat.glob('glibc-*'))
            shutil.copyfile(str(args.libc_deb), str(cache))
        else:
            assert available and not error, (available, error)
        worker, error, actual = open_engine()
        assert worker and actual == str(node), (error, actual)
        def call(method, **values):
            result = json.loads(take(lib.dc_worker_call(worker, method.encode(), json.dumps(values).encode(), None)))
            assert result['ok'], result
            return result['data']

        try:
            info = call('system.info')
            assert info['node'] == '26.13.0' and info['cordis'] == '4.0.5', info
            package = home / 'plugin'
            package.mkdir()
            (package / 'index.ts').write_text("""import type {Context} from '@deepseek-ai/cordis';
import {execFileSync} from 'node:child_process';
export function apply(ctx:Context){ctx.dunlin.registerCommand(ctx,'child',()=>{
 const program='require("node:child_process").execFileSync(process.execPath,["-e","process.stdout.write(process.versions.node)"],{stdio:"inherit"})';
 return {node:execFileSync(process.execPath,['-e',program],{encoding:'utf8'})}
});ctx.dunlin.registerCommand(ctx,'hello',(args:{name:string})=>({text:'Hello '+args.name}));
ctx.dunlin.contribute(ctx,'skin',{kind:'theme',name:'Test',tokens:{accent:'#7755ff'}})}
""")
            call('plugins.activate', id='fixture', root=str(package), entry='index.ts', config={})
            item = call('plugins.contributions')[0]
            assert item['kind'] == 'theme', item
            result = call('plugins.command', plugin='fixture', command=item['id'].split(':')[0]+':hello',
                          arguments={'name':'Dunlin'})
            assert result['text'] == 'Hello Dunlin', result
            child = call('plugins.command', plugin='fixture', command=item['id'].split(':')[0]+':child', arguments={})
            assert child['node'] == '26.13.0', child
            call('plugins.deactivate', id='fixture')
            assert not call('plugins.contributions')
        finally:
            lib.dc_worker_close(worker)
        if args.libc_deb:
            cache.unlink()
        assert open_engine(True) == (1, None, str(node))
        worker, error, actual = open_engine()
        assert worker and actual == str(node), error
        lib.dc_worker_close(worker)
        if args.libc_deb:
            runtime = next(compat.glob('glibc-*'))
            assert len(list(runtime.iterdir())) == 11
            assert (runtime / 'COPYRIGHT').read_bytes()
            rogue = runtime / 'extra.so'
            rogue.write_bytes(b'unexpected')
            worker, error, _ = open_engine()
            assert not worker and error == 'engine_private_runtime_invalid', error
            rogue.unlink()
            (runtime / 'libdl.so.2').write_bytes(b'corrupt library')
            worker, error, _ = open_engine()
            assert not worker and error == 'engine_private_runtime_invalid', error
        else:
            assert not list(compat.glob('glibc-*'))
        print('PASS Node/Cordis/TypeScript command + lifecycle and engine reuse;',
              'Bionic verified private loader, corrupt package/object and unexpected object rejected' if args.libc_deb else
              'native loader on newer glibc')


if __name__ == '__main__':
    main()
