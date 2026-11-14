// Run with Node's TypeScript support and the extracted published WebM directory.
import assert from 'node:assert/strict'
import {readFile, readdir} from 'node:fs/promises'
import path from 'node:path'
import {alphaVideo} from '../compat/dsh/web/pet-alpha.ts'

const directory = process.argv[2]
const clips = (await readdir(directory)).filter(name => name.endsWith('.webm'))
assert.ok(clips.length, 'supply an extracted animation directory')
for (const name of clips) {
  const original = await readFile(path.join(directory, name))
  const mask = alphaVideo(original)
  assert.equal(mask?.type, 'video/webm', name)
  assert.ok(mask.size > 128 && mask.size < original.length, name)
  assert.equal(alphaVideo(new Uint8Array(await mask.arrayBuffer())), undefined, 'mask must be opaque')
}
assert.throws(() => alphaVideo(Uint8Array.of(0x1a)), /Invalid WebM/)
console.log('PASS real alpha track extracted from', clips.length, 'published clips')
