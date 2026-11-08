import {Context} from './vendor/cordis/src/index.ts'
const ctx = new Context()
const log = x => console.log(x)

// Make all await boundaries deterministic using explicit Promise gates.
let release
function gate() { return new Promise(resolve => { release = resolve }) }
ctx.on('em', async () => { log('emit:a-start'); await gate(); log('emit:a-end') })
ctx.on('em', () => log('emit:b'))
ctx.emit('em')
log('emit:return')
release()
await Promise.resolve()

ctx.on('sr', async () => { log('serial:a-start'); await gate(); log('serial:a-end'); return false })
ctx.on('sr', () => { log('serial:b'); return 0 })
ctx.on('sr', () => log('unexpected'))
const serial = ctx.serial('sr')
log('serial:pending')
release()
log('serial:result=' + await serial)

let finish1, finish2
ctx.on('pr', async () => {
  log('parallel:1-start'); await new Promise(r => {finish1 = r}); log('parallel:1-end'); throw Error('first')
})
ctx.on('pr', async () => {
  log('parallel:2-start'); await new Promise(r => {finish2 = r}); log('parallel:2-end'); throw Error('second')
})
const parallel = ctx.parallel('pr')
log('parallel:pending')
finish2()
await Promise.resolve()
finish1()
try { await parallel } catch(e) { log('parallel:errors=' + e.errors.map(e => e.message).join(',')) }

for (const [label, value] of [['null',null], ['undefined',undefined], ['false',false], ['empty','']])
  ctx.on('ba', () => { log('bail:' + label); return value })
ctx.on('ba', () => log('unexpected'))
log('bail:result=' + JSON.stringify(ctx.bail('ba')))
ctx.on('promise', async () => false)
ctx.on('promise', () => log('unexpected'))
log('bail:future=' + await ctx.bail('promise'))

ctx.on('wf', next => { log('water:in2'); const x = next(); log('water:out2'); return x + 3 })
ctx.on('wf', async next => { log('water:in1'); await gate(); const x = await next(); log('water:out1'); return x * 2 }, true)
const w = ctx.waterfall('wf', () => { log('water:inner'); return 4 })
log('water:pending')
release()
log('water:result=' + await w)
ctx.on('veto', () => 'blocked')
log('water:veto=' + ctx.waterfall('veto', () => log('unexpected')))
ctx.on('twice', next => next() + next())
ctx.on('twice', () => 7)
log('water:twice=' + ctx.waterfall('twice', () => 2))

const fiber = ctx.inject(['dep'], child => {
  log('consumer:start')
  child.on('owned', () => log('consumer:hook'))
  child.effect(() => () => log('consumer:clean1'))
  child.effect(() => () => log('consumer:clean2'))
})
log('consumer:waiting')
const provider = await ctx.plugin(child => { child.provide('dep', {}); log('provider:start') })
await fiber
ctx.emit('owned')
await provider.dispose()
await fiber.inertia
ctx.emit('owned')
log('consumer:waiting-again')
const second = await ctx.plugin(child => { child.provide('dep', {}); log('provider:again') })
await fiber
ctx.emit('owned')
await fiber.dispose()
ctx.emit('owned')
await second.dispose()
log('consumer:gone')
