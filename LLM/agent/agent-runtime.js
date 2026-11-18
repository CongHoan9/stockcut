/* Bit-LLM video-agent protocol adapter. Whitespace DSL -> validated JSON. */
const MODES = new Set(['mode_action', 'mode_explain', 'mode_chat', 'mode_clarify', 'mode_recall']);
const ITEM_TYPES = new Set(['image', 'effect', 'text', 'lyric', 'audio', 'background', 'div']);
const FITS = new Set(['cover', 'contain', 'fill', 'none', 'scale-down']);
const EFFECT_RE = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const ASSET_RE = /(?:\/[\w.@+-]+(?:\/[\w.@+-]+)*\.(?:png|jpe?g|webp|gif|mp3|wav|ogg|mp4|webm|svg|ttf|otf)\b)|(?<![\/\w])[\w@+-]+(?:\.[\w@+-]+)*\.(?:png|jpe?g|webp|gif|mp3|wav|ogg|mp4|webm|svg|ttf|otf)\b/gi;
const FILE_NAMES = {canvas_compositor_js:'canvas-compositor.js', slot_tracker_cpp:'slot_tracker.cpp', dirty_rect_cpp:'dirty_rect.cpp'};
const TEXT_IDENTIFIERS = Object.freeze({
  ident_slot_state_tracker: 'SlotStateTracker', ident_display_list: 'DisplayList',
  ident_dirty_coalesce: 'dirty_coalesce', ident_gl_scissor: 'gl.scissor',
  ident_scene_fbo: 'sceneFbo', ident_gpu: 'GPU', ident_fxaa: 'FXAA',
  ident_json: 'JSON', ident_ram: 'RAM', ident_webassembly: 'WebAssembly', ident_full: 'FULL'
});
const PUNCTUATION = Object.freeze({
  punc_comma:',', punc_period:'.', punc_decimal:'.', punc_question:'?', punc_exclamation:'!',
  punc_colon:':', punc_semicolon:';', punc_dash:'-', punc_slash:'/',
  punc_open_paren:'(', punc_close_paren:')', punc_open_quote:'“', punc_close_quote:'”',
  punc_newline:'\n'
});

// The current core normalizes case and splits punctuation. Text is trained using
// visible word-markers so the client can restore ordinary prose after inference.
export function renderMarkedText(text) {
  const ts = tokens(text); let out = '', capNext = false, tightNext = false, quoteOpen = false;
  for (const raw of ts) {
    if (raw === 'punc_cap') { capNext = true; continue; }
    if (Object.hasOwn(PUNCTUATION, raw)) {
      const mark = PUNCTUATION[raw];
      if (raw === 'punc_newline') { out = out.replace(/[ \t]+$/u, '') + '\n'; tightNext = true; continue; }
      if (raw === 'punc_decimal') { out = out.replace(/[ \t]+$/u, '') + '.'; tightNext = true; continue; }
      if (raw === 'punc_open_paren' || raw === 'punc_open_quote') {
        if (raw === 'punc_open_quote') quoteOpen = true;
        out = out.replace(/[ \t]+$/u, ''); out += mark; tightNext = true; continue;
      }
      if (raw === 'punc_close_quote') {
        out = out.replace(/[ \t]+$/u, ''); out += mark; quoteOpen = false; tightNext = false; continue;
      }
      if (raw === 'punc_close_paren') { out = out.replace(/[ \t]+$/u, '') + mark; tightNext = false; continue; }
      if (raw === 'punc_dash' || raw === 'punc_slash') {
        out = out.replace(/[ \t]+$/u, '') + mark; tightNext = true; continue;
      }
      out = out.replace(/[ \t]+$/u, '') + mark; tightNext = false;
      if (raw === 'punc_period' || raw === 'punc_question' || raw === 'punc_exclamation') capNext = true;
      continue;
    }
    let word = Object.hasOwn(TEXT_IDENTIFIERS, raw) ? TEXT_IDENTIFIERS[raw] : (/^ident_hex_([0-9a-f]{3,8})$/u.test(raw) ? '#' + raw.slice('ident_hex_'.length) : raw);
    if (capNext) { word = word.length ? word[0].toLocaleUpperCase('vi-VN') + word.slice(1) : word; capNext = false; }
    if (out && !/[\s(“/-]$/u.test(out) && !tightNext) out += ' ';
    out += word;
    tightNext = false;
  }
  return out.replace(/[ \t]+/gu, ' ').trim();
}
const CORE_SEPARATORS = new Set([' ', '\t', '\n', '\r', ',', '.', '?', '!', ';', ':', '"', "'", '(', ')', '[', ']', '{', '}', '-', '/', '\\']);

export function countCoreTokens(text) {
  let count = 0, inside = false;
  for (const ch of String(text)) {
    if (CORE_SEPARATORS.has(ch)) inside = false;
    else if (!inside) { count++; inside = true; }
  }
  return count;
}

function flattenForBudget(text, limit, keepTail = false) {
  if (limit <= 0) return '';
  const words = String(text).split(/[ \t\n\r,.?!;:"'()[\]{}\-/\\]+/u).filter(Boolean);
  if (words.length <= limit) return String(text).trim();
  return (keepTail ? words.slice(-limit) : words.slice(0, limit)).join(' ');
}

/**
 * Builds a budgeted context prompt. The current request is never dropped.
 * `history` items are {user, assistant}; memories can be strings or {text, score}.
 * The max is the core's question-token budget, not a tokenizer-independent token count.
 */
export function composeAgentPrompt(currentUserText, {history = [], memories = [], maxTokens = 96} = {}) {
  if (!Number.isInteger(maxTokens) || maxTokens < 1 || maxTokens > 96) throw new RangeError('maxTokens must be an integer between 1 and the core limit 96');
  const current = `current_user ${String(currentUserText ?? '')}`;
  const currentCost = countCoreTokens(current);
  if (currentCost > maxTokens) throw new RangeError(`Current request uses ${currentCost - 1} core tokens; maximum budget is ${maxTokens - 1}`);
  let budget = maxTokens - currentCost;
  const memoryBlocks = [];
  const sortedMemories = [...memories].map(m => typeof m === 'string' ? {text:m,score:0} : m)
    .filter(m => m && typeof m.text === 'string' && m.text.trim())
    .sort((a,b) => (Number(b.score)||0) - (Number(a.score)||0));
  for (const memory of sortedMemories) {
    if (budget <= 0) break;
    const prefix = 'memory ';
    const room = budget - countCoreTokens(prefix);
    if (room <= 0) break;
    const body = flattenForBudget(memory.text, room);
    if (!body) continue;
    const block = prefix + body;
    const cost = countCoreTokens(block);
    if (cost <= budget) { memoryBlocks.push(block); budget -= cost; }
  }
  const chosenHistory = [];
  for (let i = history.length - 1; i >= 0 && budget > 0; i--) {
    const turn = history[i];
    if (!turn || typeof turn.user !== 'string') continue;
    const userPart = String(turn.user);
    const assistantPart = String(turn.assistant ?? '');
    const full = `history user ${userPart} assistant ${assistantPart}`;
    const cost = countCoreTokens(full);
    let block = full;
    if (cost > budget) {
      const fixedCost = countCoreTokens('history user assistant');
      if (budget <= fixedCost) continue;
      const bodyBudget = budget - fixedCost;
      const userBudget = Math.max(1, Math.min(bodyBudget, Math.ceil(bodyBudget * 0.6)));
      const clippedUser = flattenForBudget(userPart, userBudget, true);
      const prefix = `history user ${clippedUser} assistant`;
      const assistantBudget = Math.max(0, budget - countCoreTokens(prefix));
      const clippedAssistant = assistantBudget ? flattenForBudget(assistantPart, assistantBudget, true) : '';
      block = `${prefix}${clippedAssistant ? ' ' + clippedAssistant : ''}`;
      if (countCoreTokens(block) > budget) continue;
    }
    const used = countCoreTokens(block);
    if (used) { chosenHistory.push(block); budget -= used; }
  }
  chosenHistory.reverse();
  return [...memoryBlocks, ...chosenHistory, current].join(' ').trim();
}

const ITEM_KEYS = {
  type:'type', effect:'effect', source:'source', start_ms:'start', end_ms:'end', z_index:'z-index',
  width_pct:'width', height_pct:'height', left_pct:'left', top_pct:'top', width_px:'width', height_px:'height', left_px:'left', top_px:'top',
  object_fit:'objectFit', fade_ms:'fade', color_hex:'color', speed_milli:'speed', density_milli:'density',
  blur_milli:'blur', tolerance_milli:'tolerance', intensity_milli:'intensity', threshold_milli:'threshold',
  radius_milli:'radius', bloom_factor_milli:'bloomFactor', subpix_milli:'subpix',
  edge_threshold_milli:'edgeThreshold', edge_threshold_min_milli:'edgeThresholdMin'
};

function num(v, name) { if (!/^-?\d+(?:\.\d+)?$/.test(String(v))) throw Error(`Expected numeric ${name}`); const n=Number(v); if (!Number.isFinite(n)) throw Error(`Non-finite ${name}`); return n; }
function milli(v, name) { return num(v,name)/1000; }
function x10000(v, name) { return num(v,name)/10000; }
function tokens(s) { return String(s).trim().split(/\s+/u).filter(Boolean); }
function assetSpans(text) { const out=[]; for (const m of String(text).matchAll(ASSET_RE)) if (!out.includes(m[0])) out.push(m[0]); return out; }
function keyValues(ts, start) {
  if ((ts.length-start)%2) throw Error(`Dangling key without value: ${ts.at(-1)}`);
  const o=Object.create(null);
  for(let i=start;i<ts.length;i+=2) { if(Object.hasOwn(o,ts[i])) throw Error(`Duplicate key '${ts[i]}'`); o[ts[i]]=ts[i+1]; }
  return o;
}
function itemField(k,v,userText) {
  if(k==='src_ref') { const i=num(v,k); const spans=assetSpans(userText); if(!Number.isInteger(i)||i<0||i>=spans.length) throw Error(`src_ref ${v} does not resolve to a media path in the user request`); return ['src',spans[i]]; }
  if(k==='effect') return ['effect',v.replaceAll('_','-')];
  if(k==='object_fit') { if(!FITS.has(v)) throw Error(`Invalid object-fit '${v}'`); return ['objectFit',v]; }
  if(k==='color_hex') { if(!/^[0-9a-f]{3}(?:[0-9a-f]{3})?$/i.test(v)) throw Error('color_hex must contain 3 or 6 hex digits'); return ['color','#'+v]; }
  if(k.endsWith('_x10000')) { const base=k.slice(0,-7)+'_milli'; if(!Object.hasOwn(ITEM_KEYS,base)) throw Error(`Unknown numeric field '${k}'`); return [ITEM_KEYS[base],x10000(v,k)]; }
  if(k.endsWith('_milli')) { if(!Object.hasOwn(ITEM_KEYS,k)) throw Error(`Unknown numeric field '${k}'`); return [ITEM_KEYS[k],milli(v,k)]; }
  if(k.endsWith('_pct')) { if(!Object.hasOwn(ITEM_KEYS,k)) throw Error(`Unknown percentage field '${k}'`); return [ITEM_KEYS[k],`${num(v,k)}%`]; }
  if(k.endsWith('_px')) { if(!Object.hasOwn(ITEM_KEYS,k)) throw Error(`Unknown pixel field '${k}'`); return [ITEM_KEYS[k],`${num(v,k)}px`]; }
  if(!Object.hasOwn(ITEM_KEYS,k)) throw Error(`Unsupported item field '${k}'`);
  if(['start_ms','end_ms','z_index','fade_ms'].includes(k)) return [ITEM_KEYS[k],num(v,k)];
  if(k==='type' && !ITEM_TYPES.has(v)) throw Error(`Unsupported item type '${v}'`);
  return [ITEM_KEYS[k],v];
}
function compileAction(ts,userText) {
  if(ts.length<2) throw Error('Missing action name');
  const action=ts[1];
  if(!['set_config','add_item','update_items','delete_items'].includes(action)) throw Error(`Unsupported action '${action}'`);
  const p=keyValues(ts,2);
  if(action==='set_config') {
    const config={};
    for(const [k,v] of Object.entries(p)) { if(k!=='config_seed') throw Error(`Unsupported config key '${k}'`); const seed=num(v,k); if(!Number.isInteger(seed)||seed<0||seed>0xffffffff) throw Error('Seed must be uint32'); config.seed=seed; }
    if(!Object.keys(config).length) throw Error('set_config requires config_seed');
    return {action,config};
  }
  if(action==='add_item') {
    const item={};
    for(const [k,v] of Object.entries(p)) { const [kk,vv]=itemField(k,v,userText); if(Object.hasOwn(item,kk)) throw Error(`Multiple fields map to '${kk}'`); item[kk]=vv; }
    if(!item.type) throw Error('add_item requires type');
    if(item.start!==undefined && (!Number.isFinite(item.start)||item.start<0)) throw Error('Invalid start');
    if(item.end!==undefined && (!Number.isFinite(item.end)||item.end<0)) throw Error('Invalid end');
    if(item.start!==undefined&&item.end!==undefined&&item.end<=item.start) throw Error('end must be greater than start');
    if(['image','audio'].includes(item.type)&&!item.src) throw Error(`${item.type} requires src_ref`);
    if(item.type==='effect'&&(!item.effect||!EFFECT_RE.test(item.effect))) throw Error('effect item requires valid effect');
    if(item.color&&!/^#[0-9a-f]{3}(?:[0-9a-f]{3})?$/i.test(item.color)) throw Error('Invalid color');
    return {action,item};
  }
  const where={},set={};
  for(const [k,v] of Object.entries(p)) {
    if(k==='where_type') { if(!ITEM_TYPES.has(v)) throw Error(`Invalid selector type '${v}'`); where.type=v; }
    else if(k==='where_effect') { const e=v.replaceAll('_','-'); if(!EFFECT_RE.test(e)) throw Error('Invalid effect selector'); where.effect=e; }
    else if(k==='where_id') { if(!/^[A-Za-z0-9_-]{1,128}$/.test(v)) throw Error('Invalid item id'); where.id=v; }
    else if(k.startsWith('set_')) {
      const f=k.slice(4), map={
        radius_milli:'radius',speed_milli:'speed',density_milli:'density',blur_milli:'blur',tolerance_milli:'tolerance',
        intensity_milli:'intensity',threshold_milli:'threshold',bloom_factor_milli:'bloomFactor',subpix_milli:'subpix',
        edge_threshold_milli:'edgeThreshold',edge_threshold_min_milli:'edgeThresholdMin',
        radius_x10000:'radius',speed_x10000:'speed',density_x10000:'density',blur_x10000:'blur',tolerance_x10000:'tolerance',
        intensity_x10000:'intensity',threshold_x10000:'threshold',bloom_factor_x10000:'bloomFactor',subpix_x10000:'subpix',
        edge_threshold_x10000:'edgeThreshold',edge_threshold_min_x10000:'edgeThresholdMin',
        fade_ms:'fade',z_index:'z-index',object_fit:'objectFit',start_ms:'start',end_ms:'end',color_hex:'color',source:'source',
        width_pct:'width',height_pct:'height',left_pct:'left',top_pct:'top',width_px:'width',height_px:'height',left_px:'left',top_px:'top'
      };
      if(!Object.hasOwn(map,f)) throw Error(`Unsupported update field '${f}'`);
      if(f==='color_hex') set[map[f]]=itemField(f,v,userText)[1];
      else if(f.endsWith('_x10000')) set[map[f]]=x10000(v,k);
      else if(f.endsWith('_milli')) set[map[f]]=milli(v,k);
      else if(f.endsWith('_pct')) set[map[f]]=`${num(v,k)}%`;
      else if(f.endsWith('_px')) set[map[f]]=`${num(v,k)}px`;
      else if(['fade_ms','z_index','start_ms','end_ms'].includes(f)) set[map[f]]=num(v,k);
      else if(f==='object_fit') { if(!FITS.has(v)) throw Error(`Invalid object-fit '${v}'`); set[map[f]]=v; }
      else set[map[f]]=v;
    } else throw Error(`Unsupported selector/update field '${k}'`);
  }
  if(!Object.keys(where).length) throw Error(`${action} requires a selector`);
  if(action==='update_items'&&!Object.keys(set).length) throw Error('update_items requires set fields');
  return action==='update_items'?{action,where,set}:{action,where};
}
function compileExplain(ts) {
  let topic=null,files=[],answerAt=-1;
  for(let i=1;i<ts.length;i++) {
    if(ts[i]==='answer') { answerAt=i; break; }
    if(ts[i]==='topic') { topic=ts[++i]; if(!topic) throw Error('Missing explain topic'); }
    else if(ts[i]==='files') { i++; while(i<ts.length&&ts[i]!=='answer') files.push(ts[i++]); if(ts[i]==='answer'){answerAt=i;break;} i--; }
    else throw Error(`Unexpected explain token '${ts[i]}'`);
  }
  if(!topic||answerAt<0||answerAt===ts.length-1) throw Error('mode_explain requires topic, files and answer text');
  return {type:'explain',topic,files:files.filter(f=>f!=='none').map(f=>Object.hasOwn(FILE_NAMES,f)?FILE_NAMES[f]:f.replaceAll('_','-')),answer:renderMarkedText(ts.slice(answerAt+1).join(' '))};
}

export function validateActionShape(a) {
  const bad = reason => ({valid:false,reason});
  if(!a||typeof a!=='object'||Array.isArray(a)||typeof a.action!=='string') return bad('Action must be an object with action string');
  if(a.action==='set_config') {
    if(!a.config||typeof a.config!=='object'||Array.isArray(a.config)||Object.keys(a.config).some(k=>k!=='seed')) return bad('config only supports seed');
    const seed=a.config.seed;
    return Number.isInteger(seed)&&seed>=0&&seed<=0xffffffff?{valid:true,requiresHostCheck:false}:bad('Invalid uint32 seed');
  }
  if(a.action==='add_item') {
    const i=a.item;
    if(!i||typeof i!=='object'||Array.isArray(i)||!ITEM_TYPES.has(i.type)) return bad('Unsupported or missing item.type');
    const itemKeys=new Set(['type','src','effect','source','start','end','z-index','width','height','left','top','objectFit','fade','color','speed','density','blur','tolerance','intensity','threshold','radius','bloomFactor','subpix','edgeThreshold','edgeThresholdMin']);
    const unknown=Object.keys(i).find(k=>!itemKeys.has(k)); if(unknown) return bad(`Unsupported item field '${unknown}'`);
    if(i.start!==undefined&&(!Number.isFinite(i.start)||i.start<0)) return bad('Invalid item.start');
    if(i.end!==undefined&&(!Number.isFinite(i.end)||i.end<0)) return bad('Invalid item.end');
    if(i.start!==undefined&&i.end!==undefined&&i.end<=i.start) return bad('item.end must be greater than item.start');
    if(i['z-index']!==undefined&&!Number.isInteger(i['z-index'])) return bad('item.z-index must be an integer');
    if(['image','audio'].includes(i.type)&&(typeof i.src!=='string'||!i.src)) return bad(`${i.type} requires src`);
    if(i.type==='effect'&&(typeof i.effect!=='string'||!EFFECT_RE.test(i.effect))) return bad('Invalid effect identifier');
    if(i.objectFit!==undefined&&!FITS.has(i.objectFit)) return bad('Invalid objectFit');
    if(i.source!==undefined&&(typeof i.source!=='string'||!i.source)) return bad('Invalid item.source');
    for(const key of ['fade','speed','density','blur','tolerance','intensity','threshold','radius','bloomFactor','subpix','edgeThreshold','edgeThresholdMin'])
      if(i[key]!==undefined&&(typeof i[key]!=='number'||!Number.isFinite(i[key]))) return bad(`Invalid item.${key}`);
    for(const key of ['width','height','left','top']) if(i[key]!==undefined&&typeof i[key]!=='string') return bad(`item.${key} must be a CSS string`);
    if(i.color!==undefined&&(typeof i.color!=='string'||!/^#[0-9a-f]{3}(?:[0-9a-f]{3})?$/i.test(i.color))) return bad('Invalid item.color');
    return {valid:true,requiresHostCheck:true};
  }
  if(a.action==='update_items'||a.action==='delete_items') {
    if(!a.where||typeof a.where!=='object'||Array.isArray(a.where)||!Object.keys(a.where).length) return bad(`${a.action} requires a selector`);
    const selectorKeys=new Set(['id','type','effect']);
    const unknownSelector=Object.keys(a.where).find(k=>!selectorKeys.has(k)); if(unknownSelector) return bad(`Unsupported selector '${unknownSelector}'`);
    if(a.where.id!==undefined&&(typeof a.where.id!=='string'||!a.where.id)) return bad('Selector id must be a non-empty string');
    if(a.where.type!==undefined&&!ITEM_TYPES.has(a.where.type)) return bad('Unsupported selector type');
    if(a.where.effect!==undefined&&(typeof a.where.effect!=='string'||!EFFECT_RE.test(a.where.effect))) return bad('Invalid selector effect');
    if(a.action==='delete_items') return {valid:true,requiresHostCheck:true};
    if(!a.set||typeof a.set!=='object'||Array.isArray(a.set)||!Object.keys(a.set).length) return bad('update_items requires set fields');
    const setKeys=new Set(['radius','speed','density','blur','tolerance','intensity','threshold','bloomFactor','subpix','edgeThreshold','edgeThresholdMin','fade','z-index','objectFit','start','end','color','width','height','left','top','source']);
    const unknownSet=Object.keys(a.set).find(k=>!setKeys.has(k)); if(unknownSet) return bad(`Unsupported update field '${unknownSet}'`);
    if(a.set.source!==undefined&&(typeof a.set.source!=='string'||!a.set.source)) return bad('Invalid set.source');
    for(const key of ['radius','speed','density','blur','tolerance','intensity','threshold','bloomFactor','subpix','edgeThreshold','edgeThresholdMin','fade']) if(a.set[key]!==undefined&&(typeof a.set[key]!=='number'||!Number.isFinite(a.set[key]))) return bad(`Invalid set.${key}`);
    if(a.set['z-index']!==undefined&&!Number.isInteger(a.set['z-index'])) return bad('set.z-index must be an integer');
    if(a.set.color!==undefined&&(typeof a.set.color!=='string'||!/^#[0-9a-f]{3}(?:[0-9a-f]{3})?$/i.test(a.set.color))) return bad('Invalid set.color');
    if(a.set.objectFit!==undefined&&!FITS.has(a.set.objectFit)) return bad('Invalid set.objectFit');
    for(const key of ['width','height','left','top']) if(a.set[key]!==undefined&&typeof a.set[key]!=='string') return bad(`set.${key} must be a CSS string`);
    if(a.set.start!==undefined&&(!Number.isFinite(a.set.start)||a.set.start<0)) return bad('Invalid set.start');
    if(a.set.end!==undefined&&(!Number.isFinite(a.set.end)||a.set.end<0)) return bad('Invalid set.end');
    if(a.set.start!==undefined&&a.set.end!==undefined&&a.set.end<=a.set.start) return bad('set.end must be greater than set.start');
    return {valid:true,requiresHostCheck:true};
  }
  return bad(`Action '${a.action}' is not allowed`);
}

export function parseAgentResponse(raw,userText='',options={}) {
  const s=String(raw??'').trim(); if(!s) return {type:'error',code:'empty_output',message:'Empty model output'};
  const cut=s.search(/\s/u), first=cut<0?s:s.slice(0,cut);
  if(MODES.has(first)) {
    try {
      if(first==='mode_chat') return {type:'chat',text:cut<0?'':renderMarkedText(s.slice(cut+1))};
      if(first==='mode_clarify') return {type:'clarify',question:cut<0?'':renderMarkedText(s.slice(cut+1))};
      if(first==='mode_recall') { const ts=tokens(s); if(ts[1]!=='query'||ts.length<3) throw Error('mode_recall requires query text'); return {type:'recall',query:renderMarkedText(ts.slice(2).join(' ')),raw:s}; }
      if(first==='mode_explain') return compileExplain(tokens(s));
      const payload=compileAction(tokens(s),options.sourceText??userText), shape=validateActionShape(payload);
      if(!shape.valid) return {type:'error',code:'invalid_action',message:shape.reason,raw:s};
      const r={type:'action',payload,schemaValid:true,readyToExecute:false,requiresHostCheck:!!shape.requiresHostCheck,raw:s};
      if(typeof options.validateAction==='function') {
        const host=options.validateAction(payload,{userText,raw:s});
        if(host&&typeof host.then==='function') return {...r,hostValidation:'async_required',message:'Use runAgentTurn for asynchronous validation'};
        const ok=host===true||host?.valid===true; r.readyToExecute=ok; r.hostValidation=ok?'passed':'rejected'; if(!ok) r.message=host?.reason||'Host/editor validation rejected action';
      }
      return r;
    } catch(e) { return {type:'error',code:'invalid_agent_dsl',message:e instanceof Error?e.message:String(e),raw:s}; }
  }
  if(s.startsWith('{')||s.startsWith('[')) {
    let o; try{o=JSON.parse(s);}catch(e){return {type:'error',code:'malformed_json',message:`Refusing malformed structured output: ${e.message}`,raw:s};}
    if(o?.type==='explain'&&typeof o.answer==='string')return o;
    if(o?.type==='chat'&&typeof o.text==='string')return o;
    if(o?.type==='clarify'&&typeof o.question==='string')return o;
    const shape=validateActionShape(o); if(!shape.valid)return {type:'error',code:'invalid_action',message:shape.reason,raw:s};
    if(o.action==='add_item'&&o.item?.src&&!assetSpans(options.sourceText??userText).includes(o.item.src)) return {type:'error',code:'untrusted_asset_path',message:'Action src must match a media path in the current user request.',raw:s};
    const r={type:'action',payload:o,schemaValid:true,readyToExecute:false,requiresHostCheck:!!shape.requiresHostCheck,raw:s};
    if(typeof options.validateAction==='function') { const host=options.validateAction(o,{userText,raw:s}); if(host&&typeof host.then==='function')return {...r,hostValidation:'async_required'}; const ok=host===true||host?.valid===true;r.readyToExecute=ok;r.hostValidation=ok?'passed':'rejected';if(!ok)r.message=host?.reason||'Host/editor validation rejected action'; }
    return r;
  }
  return {type:'chat',text:s,legacy:true};
}

export async function runAgentTurn(Module,userText,options={}) {
  const currentText = String(userText ?? '');
  const maxTokens = Number.isInteger(options.maxQuestionTokens) ? Math.min(96, Math.max(1, options.maxQuestionTokens)) : 96;
  let prompt = options.prompt ?? ((options.history?.length || options.memories?.length)
    ? composeAgentPrompt(currentText,{history:options.history||[],memories:options.memories||[],maxTokens})
    : currentText);
  if (countCoreTokens(prompt) > 96) throw new RangeError(`Inference prompt exceeds core limit of 96 tokens (got ${countCoreTokens(prompt)})`);
  const maxBytes = Number.isInteger(options.maxInputBytes) ? options.maxInputBytes : 4095;
  const fn=n=>{const f=Module[`_wasm_bit_llm_${n}`];if(typeof f!=='function')throw Error(`Missing WASM export wasm_bit_llm_${n}`);return f;};
  const inferOnce = text => {
    const bytes = new TextEncoder().encode(text);
    if (bytes.byteLength >= maxBytes) throw new RangeError(`Input exceeds ${maxBytes-1} bytes`);
    if (!Module.HEAPU8) throw Error('WASM HEAPU8 unavailable');
    const ptr=fn('get_input_ptr')(); let heap=Module.HEAPU8; heap.set(bytes,ptr); heap[ptr+bytes.length]=0;
    const len=fn('infer')(bytes.length), outPtr=fn('get_output_ptr')(); heap=Module.HEAPU8;
    const raw=new TextDecoder().decode(heap.slice(outPtr,outPtr+Math.max(0,Math.min(Number(len)||0,4095))));
    return {raw,outputLength:len};
  };
  const maxRecallTurns = Number.isInteger(options.maxRecallTurns) ? Math.max(0,Math.min(4,options.maxRecallTurns)) : 2;
  let result, raw = '', outputLength = 0, recallTurns = 0;
  for (;;) {
    ({raw,outputLength}=inferOnce(prompt));
    result=parseAgentResponse(raw,currentText,{sourceText:currentText});
    if (result.type !== 'recall') break;
    if (!options.contextMemory || typeof options.contextMemory.search !== 'function') {
      return {...result,type:'error',code:'recall_unavailable',message:'Model requested context lookup, but no contextMemory.search provider was supplied.',raw,outputLength,recallTurns};
    }
    if (recallTurns >= maxRecallTurns) {
      return {type:'error',code:'recall_limit',message:`Context lookup limit (${maxRecallTurns}) reached.`,raw,outputLength,recallTurns};
    }
    const hits = await options.contextMemory.search(result.query, Number.isInteger(options.recallTopK)?options.recallTopK:5);
    const memories = [...(options.memories||[]), ...(hits||[]).map(h=>({text:h.text,score:h.score||0}))];
    if (!hits?.length) memories.push({text:`No stored memory matched: ${result.query}`,score:-1});
    prompt = composeAgentPrompt(currentText,{history:options.history||[],memories,maxTokens});
    recallTurns++;
  }

  if (result.type === 'action') {
    if (typeof options.validateAction === 'function') {
      try {
        const host = await options.validateAction(result.payload,{userText:currentText,raw,prompt});
        const ok = host === true || host?.valid === true;
        result = {...result,readyToExecute:ok,hostValidation:ok?'passed':'rejected',hostValidationResult:host};
        if (!ok) result.message = host?.reason || 'Host/editor validation rejected action';
      } catch (e) {
        result = {...result,readyToExecute:false,hostValidation:'error',message:e instanceof Error?e.message:String(e)};
      }
    } else {
      result = {...result,readyToExecute:false,hostValidation:'missing',message:'Action is parsed but cannot execute until validateAction confirms the current editor state.'};
    }
    if (result.readyToExecute && typeof options.executeAction === 'function') {
      try { result = {...result,executed:true,executionResult:await options.executeAction(result.payload)}; }
      catch(e) { result = {...result,executed:false,executionError:e instanceof Error?e.message:String(e)}; }
    }
  }

  if (options.remember === true && options.contextMemory && typeof options.contextMemory.add === 'function') {
    let responseText = '';
    if (result.type === 'chat') responseText = result.text;
    else if (result.type === 'explain') responseText = `explain ${result.topic} ${result.answer}`;
    else if (result.type === 'clarify') responseText = `clarify ${result.question}`;
    else if (result.type === 'action') responseText = `action ${JSON.stringify(result.payload)}${result.executed?' executed':''}`;
    else responseText = `${result.type || 'result'} ${result.message || result.code || ''}`;
    try { await options.contextMemory.add(`turn user ${currentText} assistant ${responseText}`,{kind:1,importance:options.memoryImportance??128}); }
    catch(e) { result = {...result,memoryWriteError:e instanceof Error?e.message:String(e)}; }
  }
  return {...result,raw,outputLength,recallTurns,promptTokens:countCoreTokens(prompt)};
}

export const videoAgentProtocol=Object.freeze({version:1,modes:['mode_chat','mode_explain','mode_action','mode_clarify','mode_recall'],actionNames:['set_config','add_item','update_items','delete_items'],textMarkers:['punc_cap','punc_comma','punc_period','punc_question','punc_exclamation','punc_colon','punc_semicolon'],note:'Next-token text protocol; action DSL compiles to JSON, context recall is bounded, and every editor action must pass host validation before execution.'});
