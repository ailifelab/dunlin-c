// Named, immutable-source-bound repairs for defects in pinned upstream hosts.
// The npm package on disk is never rewritten.
import crypto from 'node:crypto';
const SESSION_TIME_SHA='2f5dd07cb5c744a29abbb8003a8a4cefd9e582c6e2538074ca299a0ddd02a3bf';
export function nativeSourceTimeRepair(source){
 if(crypto.createHash('sha256').update(source).digest('hex')!==SESSION_TIME_SHA)return null;
 const before='const event = deepFreeze({\n\t\t\ttype,\n\t\t\tseq: SessionSeq(this.log.length),\n\t\t\ttime: Date.now(),';
 const after="const sourceTime = surfaceOpts?.[Symbol.for('dunlin.native.sourceTime')];\n\t\tif (sourceTime !== undefined && (!Number.isSafeInteger(sourceTime) || sourceTime < 0)) throw new Error('Invalid native source timestamp');\n\t\tconst event = deepFreeze({\n\t\t\ttype,\n\t\t\tseq: SessionSeq(this.log.length),\n\t\t\ttime: sourceTime ?? Date.now(),";
 if(source.split(before).length!==2)throw Error('Pinned SDK source-time repair anchor changed');
 return {source:source.replace(before,after),id:'session-native-source-time',upstreamSha256:SESSION_TIME_SHA};
}
const PROJECT_SHA='af60254a389e1df99b4534152f0e866dd0d3d0c9e536fa9641c917d433e1b56e';
export function projectTitleEffortRepair(pkg,source){
 if(pkg?.name!=='dsh-project-manager'||pkg.version!=='0.57.0'||crypto.createHash('sha256').update(source).digest('hex')!==PROJECT_SHA)return null;
 const before='const r = await titleWithRetry(attempt, effort, retryEffort);';
 if(source.split(before).length!==2)throw Error('Pinned Project title-effort repair anchor changed');
 return {source:source.replace(before,"const r = await titleWithRetry(attempt, effort ?? '', retryEffort ?? '');"),id:'project-title-unset-effort',upstreamSha256:PROJECT_SHA};
}
const PROJECT_HISTORY_SHA='099e64a3e73a670bf643bfdfcef9f15059e9105f5c8e324ce32f232ccd9b91d2';
export function projectUnknownHistoryTimeRepair(pkg,source){
 if(pkg?.name!=='dsh-project-manager'||pkg.version!=='0.57.0'||crypto.createHash('sha256').update(source).digest('hex')!==PROJECT_HISTORY_SHA)return null;
 const before="if (typeof t === 'number')";
 if(source.split(before).length!==2)throw Error('Pinned Project history-time repair anchor changed');
 return {source:source.replace(before,"if (typeof t === 'number' && Number.isFinite(t) && t > 0)"),id:'project-unknown-history-time',upstreamSha256:PROJECT_HISTORY_SHA};
}
const PROJECT_DAYS_SHA='c47b2e0c542b52e2f1ef3433a2db6868b8c6c058ff8acbcc55589525e075752a';
export function projectUnknownActivityTimeRepair(pkg,source){
 if(pkg?.name!=='dsh-project-manager'||pkg.version!=='0.57.0'||crypto.createHash('sha256').update(source).digest('hex')!==PROJECT_DAYS_SHA)return null;
 const before="const days = [...new Set(evs.filter((e) => e && typeof e.time === 'number' && MSG.has(String(e.type))).map((e) => ymd(e.time)))].sort();\n                this.map.set(id, { days, at: Date.now(), ver });";
 const after="const unknown = evs.some((e) => e && MSG.has(String(e.type)) && !(Number.isSafeInteger(e.time) && e.time > 0));\n                const days = [...new Set(evs.filter((e) => e && Number.isSafeInteger(e.time) && e.time > 0 && MSG.has(String(e.type))).map((e) => ymd(e.time)))].sort();\n                const notice = unknown ? '部分历史没有逐事件时间，仅统计有可靠时间的记录' : undefined;\n                this.map.set(id, { days, at: Date.now(), ver, ...(notice ? {error: notice} : {}) });\n                if (notice) degraded.push(id.slice(0, 14) + '…：' + notice);";
 if(source.split(before).length!==2)throw Error('Pinned Project activity-time repair anchor changed');
 return {source:source.replace(before,after),id:'project-unknown-activity-time',upstreamSha256:PROJECT_DAYS_SHA};
}
const CHECKPOINT_SHA='85d5d9b06bdf4b35b8403c20d17742a79c42984af18bc106cc6154d833b277b8';
export function checkpointLiveToolRepair(pkg,source){
 if(pkg?.name!=='dsh-checkpoint-rewind'||pkg.version!=='0.6.34'||crypto.createHash('sha256').update(source).digest('hex')!==CHECKPOINT_SHA)return null;
 const edits=[
  ["  ctx.inject(['tools'], (toolsCtx) => {\n    const update = () => {\n      if (getLive().checkpointTool === true) {\n        toolsCtx.effect(() => toolsCtx.tools.register(defineTool({", "  ctx.inject(['tools'], (toolsCtx) => {\n    let dispose\n    const update = () => {\n      dispose?.()\n      dispose = undefined\n      if (getLive().checkpointTool === true) {\n        dispose = toolsCtx.effect(() => toolsCtx.tools.register(defineTool({"],
  ["    toolsCtx.effect(() => () => { unwatch() }, `${PLUGIN_NAME}.checkpoint-tool.watch`)","    toolsCtx.effect(() => () => { unwatch(); dispose?.() }, `${PLUGIN_NAME}.checkpoint-tool.watch`)"]
 ];
 let patched=source;for(const [before,after] of edits){if(patched.split(before).length!==2)throw Error('Pinned checkpoint live-tool repair anchor changed');patched=patched.replace(before,after)}return {source:patched,id:'checkpoint-live-tool-disposal',upstreamSha256:CHECKPOINT_SHA};
}

const IMAGE_SHA='a9fc05e5d42d92c8eeffe7ad65f7a805349f5b1a09b72654b7f3a9bf9f8f6fe1';
export function imageNativeWorkspaceRepair(pkg,source){
 if(pkg?.name!=='dsh-image-gen'||pkg.version!=='0.11.0'||crypto.createHash('sha256').update(source).digest('hex')!==IMAGE_SHA)return null;
 const edits=[
  ['getActiveWorkspaceRoot: () => Array.from(knownWorkspaceRoots)[0] || process.cwd(),',"getActiveWorkspaceRoot: () => ctx.get('dunlinNativeContext')?.workspace() ?? (Array.from(knownWorkspaceRoots)[0] || process.cwd()),"],
  ['getAllowedWorkspaceRoots: async () => {',"getAllowedWorkspaceRoots: async () => {\n                    const nativeRoot = ctx.get('dunlinNativeContext')?.workspace(); if (nativeRoot) return new Set([nativeRoot]);"],
  ['deleteWorkspaceImage: async (filePath) => {',"deleteWorkspaceImage: async (filePath) => {\n                const nativeRoot = ctx.get('dunlinNativeContext')?.workspace(); if (nativeRoot) return deleteImageFromWorkspace(filePath, new Set([nativeRoot]));"],
  ['const workspaces = await getDshWorkspacesFull().catch(() => []);',"const workspaces = ctx.get('dunlinNativeContext')?.workspaces() ?? await getDshWorkspacesFull().catch(() => []);"],
  ['const activeRoot = Array.from(knownWorkspaceRoots)[0] || workspaces[0]?.path || process.cwd();',"const activeRoot = ctx.get('dunlinNativeContext')?.workspace() ?? (Array.from(knownWorkspaceRoots)[0] || workspaces[0]?.path || process.cwd());"],
  ['const fallbackRoot = Array.from(knownWorkspaceRoots)[0] || process.cwd();',"const nativeRoot = ctx.get('dunlinNativeContext')?.workspace(); if (nativeRoot && input.workspaceRoot && input.workspaceRoot !== nativeRoot) throw Object.assign(new Error('Image workspace differs from native chat'), {code:-32602});\n                    const fallbackRoot = nativeRoot ?? (Array.from(knownWorkspaceRoots)[0] || process.cwd());"]
 ];
 let patched=source;for(const [before,after]of edits){if(patched.split(before).length!==2)throw Error('Pinned image workspace repair anchor changed');patched=patched.replace(before,after)}return {source:patched,id:'image-native-workspace',upstreamSha256:IMAGE_SHA};
}

export function projectCalendarRevisionRepair(pkg,source){
 const sha='458f3d074742ef94ff3282b399fe6ea2db596669a1fccd31304652da0631b3de';
 if(pkg?.name!=='dsh-project-manager'||pkg.version!=='0.57.0'||crypto.createHash('sha256').update(source).digest('hex')!==sha)return null;
 const before='const s = JSON.stringify(shown);';
 if(source.split(before).length!==2)throw Error('Pinned calendar revision anchor changed');
 return {source:source.replace(before,"const s = JSON.stringify([shown, (_dg ?? []).filter(text => !/正在统计/.test(text)).sort()]);"),id:'calendar-visible-notice-revision',upstreamSha256:sha};
}
