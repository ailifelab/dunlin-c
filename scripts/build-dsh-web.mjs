// Build-time only. Approved runtime dependencies are installed from the lockfile;
// no plugin code executes while building the bundled browser runtime.
import fs from 'node:fs/promises';import path from 'node:path';import {createRequire} from 'node:module';import {fileURLToPath} from 'node:url';import crypto from 'node:crypto';
const root=path.dirname(path.dirname(fileURLToPath(import.meta.url))),toolchain=path.resolve(root,process.argv[2]??'build/v51/dsh-web-toolchain'),out=path.resolve(root,process.argv[3]??'build/v51/dsh-web-runtime');
if(!toolchain.startsWith(root+'/build/')||!out.startsWith(root+'/build/'))throw Error('build directories must be owned');
const require=createRequire(path.join(toolchain,'package.json')),esbuild=require('esbuild');
for(const [name,version]of Object.entries(JSON.parse(await fs.readFile(path.join(root,'compat/dsh/web/package.json'),'utf8')).dependencies)){
 if(require(name+'/package.json').version!==version)throw Error('toolchain version mismatch: '+name)
}
for(const relative of ['compat/dsh','compat/dsh/web']){
 const pin=JSON.parse(await fs.readFile(path.join(root,relative,'pin.json'),'utf8'));
 for(const item of pin.files){const bytes=await fs.readFile(path.join(root,relative,item.path));if(crypto.createHash('sha256').update(bytes).digest('hex')!==item.sha256)throw Error('fixed source changed: '+relative+'/'+item.path)}
}
await fs.mkdir(out,{recursive:true});
const result=await esbuild.build({entryPoints:[path.join(root,'compat/dsh/web/runtime.ts')],outfile:path.join(out,'runtime.js'),bundle:true,format:'iife',globalName:'DunlinDSH',platform:'browser',target:'safari17',jsx:'automatic',metafile:true,nodePaths:[path.join(toolchain,'node_modules')],define:{'process.env.NODE_ENV':'"production"'},alias:{'@deepseek-ai/cordis':path.join(root,'compat/dsh/vendor/cordis/src/index.ts'),'@deepseek-ai/cosmokit':path.join(root,'compat/dsh/vendor/cosmokit/src/index.ts'),'@deepseek-ai/dsh-client-ui-slots':path.join(root,'compat/dsh/web/vendor/ui-slots/src/index.ts')}});
await fs.writeFile(path.join(out,'metafile.json'),JSON.stringify(result.metafile,null,2)+'\n');console.log('Built optional fixed DSH browser runtime:',out)
