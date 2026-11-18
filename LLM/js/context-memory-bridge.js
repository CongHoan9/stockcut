/*
 * Bit-LLM Context Memory bridge for Emscripten/WebAssembly.
 *
 * Fast path: all search/read work happens in C++ RAM. Browser persistence uses
 * an append-only IndexedDB journal, so adding one memory does not serialize the
 * entire history. A compact binary .ctx file can be imported/exported explicitly.
 *
 * Usage:
 *   const memory = new BitLLMContextMemory(Module);
 *   await memory.restoreAfterModelLoad();
 *   await memory.add('Khi focus lyrics, giữ nguyên khoảng trắng thụt lề.', {kind: 3, importance: 230});
 *   console.log(memory.search('lyrics khoảng trắng'));
 *   await memory.exportFile(); // optional portable context_memory.ctx
 */
export class BitLLMContextMemory {
  constructor(Module, options = {}) {
    this.Module = Module;
    this.dbName = options.dbName || 'bit-llm-context-memory-v2';
    this.stateStore = 'state';
    this.journalStore = 'entries';
    this.key = options.key || 'default';
    this.maxInputBytes = options.maxInputBytes || 4095;
    this.sessionId = options.sessionId ?? 0n;
    this.projectId = options.projectId ?? 0n;
    this._dbPromise = null;
    this._lastError = null;
  }

  _fn(name) {
    const fn = this.Module[`_wasm_bit_llm_${name}`];
    if (typeof fn !== 'function') throw new Error(`Missing WASM export: wasm_bit_llm_${name}`);
    return fn;
  }
  _memory() {
    if (!this.Module.HEAPU8) throw new Error('WASM HEAPU8 is unavailable');
    return this.Module.HEAPU8;
  }
  _writeInput(text) {
    const bytes = new TextEncoder().encode(String(text));
    if (bytes.length > this.maxInputBytes) throw new RangeError(`Context text is ${bytes.length} bytes; maximum is ${this.maxInputBytes}`);
    const ptr = this._fn('get_input_ptr')();
    this._memory().set(bytes, ptr);
    this._memory()[ptr + bytes.length] = 0;
    return bytes.length;
  }
  _splitU64(value) {
    const n = BigInt(value);
    if (n < 0n || n > 0xffffffffffffffffn) throw new RangeError('Scope ID must be an unsigned 64-bit integer');
    return [Number(n & 0xffffffffn), Number(n >> 32n)];
  }
  setScope(sessionId = this.sessionId, projectId = this.projectId) {
    this.sessionId = BigInt(sessionId);
    this.projectId = BigInt(projectId);
    const [sl, sh] = this._splitU64(this.sessionId);
    const [pl, ph] = this._splitU64(this.projectId);
    this._fn('context_set_scope')(sl, sh, pl, ph);
    return {sessionId: this.sessionId.toString(), projectId: this.projectId.toString()};
  }
  _splitTimestamp(ms = Date.now()) {
    // ContextMemory ranks recency in Unix seconds; the browser API accepts JS milliseconds.
    const t = BigInt(Math.max(0, Math.trunc(ms / 1000)));
    return [Number(t & 0xffffffffn), Number((t >> 32n) & 0xffffffffn)];
  }
  _addToWasm(text, {kind = 0, importance = 128, timestamp = Date.now(), sessionId = this.sessionId, projectId = this.projectId} = {}) {
    this.setScope(sessionId, projectId);
    const len = this._writeInput(text);
    const [lo, hi] = this._splitTimestamp(timestamp);
    const count = this._fn('context_add')(len, kind & 255, importance & 255, lo, hi);
    if (!count) throw new Error('WASM rejected the context entry');
    return count;
  }

  async add(text, {kind = 0, importance = 128, timestamp = Date.now(), sessionId = this.sessionId, projectId = this.projectId, persist = true} = {}) {
    const normalized = String(text);
    this._addToWasm(normalized, {kind, importance, timestamp, sessionId, projectId}); // RAM update first; retrieval is immediately available.
    if (persist) {
      try {
        await this._appendJournal({text: normalized, kind: kind & 255, importance: importance & 255, timestamp: Math.trunc(timestamp), sessionId: String(sessionId), projectId: String(projectId)});
        this._lastError = null;
      } catch (err) {
        this._lastError = err;
        throw err; // the in-RAM entry remains available in the current session.
      }
    }
    return this.count();
  }

  search(query, topK = 5, now = Date.now()) {
    this.setScope();
    const len = this._writeInput(query);
    const [lo, hi] = this._splitTimestamp(now);
    const count = this._fn('context_search')(len, Math.max(1, Math.min(64, topK | 0)), lo, hi);
    const decoder = new TextDecoder();
    const out = [];
    for (let i = 0; i < count; i++) {
      const ptr = this._fn('context_hit_text_ptr')(i);
      const textLen = this._fn('context_hit_text_len')(i);
      const text = decoder.decode(this._memory().slice(ptr, ptr + textLen));
      const id = (BigInt(this._fn('context_hit_id_hi')(i)) << 32n) | BigInt(this._fn('context_hit_id_lo')(i));
      out.push({id: id.toString(), score: this._fn('context_hit_score')(i) / 1000,
        kind: this._fn('context_hit_kind')(i), text});
    }
    return out;
  }

  count() { return this._fn('context_count')(); }
  stats() {
    return {entries: this.count(), indexedTerms: this._fn('context_indexed_terms')(), approximateRamBytes: this._fn('context_ram_bytes')()};
  }

  _exportSnapshot() {
    const total = this._fn('context_export_begin')();
    if (!total) throw new Error('Could not serialize context memory');
    const chunkPtr = this._fn('get_chunk_buffer_ptr')();
    const chunkMax = this._fn('get_chunk_buffer_size')();
    const out = new Uint8Array(total);
    for (let offset = 0; offset < total;) {
      const got = this._fn('context_export_read')(offset, Math.min(chunkMax, total - offset));
      if (!got) throw new Error(`Context export stopped at byte ${offset}`);
      out.set(this._memory().slice(chunkPtr, chunkPtr + got), offset);
      offset += got;
    }
    return out;
  }

  _importSnapshot(bytes) {
    if (!(bytes instanceof Uint8Array)) bytes = new Uint8Array(bytes);
    if (!this._fn('context_import_begin')(bytes.byteLength)) throw new Error('Context snapshot size rejected');
    const chunkPtr = this._fn('get_chunk_buffer_ptr')();
    const chunkMax = this._fn('get_chunk_buffer_size')();
    for (let offset = 0; offset < bytes.byteLength;) {
      const part = bytes.subarray(offset, Math.min(offset + chunkMax, bytes.byteLength));
      this._memory().set(part, chunkPtr);
      const got = this._fn('context_import_chunk')(offset, part.byteLength);
      if (got !== part.byteLength) throw new Error(`Context import stopped at byte ${offset}`);
      offset += got;
    }
    const count = this._fn('context_import_finalize')();
    if (!count && bytes.byteLength > 16) throw new Error('Context snapshot is invalid or corrupted');
    return count;
  }

  async _openDb() {
    if (this._dbPromise) return this._dbPromise;
    if (!globalThis.indexedDB) throw new Error('IndexedDB is unavailable in this environment');
    this._dbPromise = new Promise((resolve, reject) => {
      const req = indexedDB.open(this.dbName, 1);
      req.onupgradeneeded = () => {
        const db = req.result;
        if (!db.objectStoreNames.contains(this.stateStore)) db.createObjectStore(this.stateStore);
        if (!db.objectStoreNames.contains(this.journalStore)) db.createObjectStore(this.journalStore, {autoIncrement: true});
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error || new Error('Could not open context database'));
      req.onblocked = () => reject(new Error('Context database upgrade is blocked by another tab'));
    });
    return this._dbPromise;
  }

  async _readStateAndJournal() {
    const db = await this._openDb();
    return new Promise((resolve, reject) => {
      const tx = db.transaction([this.stateStore, this.journalStore], 'readonly');
      const stateReq = tx.objectStore(this.stateStore).get(this.key);
      const journalReq = tx.objectStore(this.journalStore).getAll();
      let stateDone = false, journalDone = false, state = null, journal = null;
      const finish = () => { if (stateDone && journalDone) {
        const scoped = (journal || []).filter(record => !record.memoryKey || record.memoryKey === this.key);
        resolve({state: state ? new Uint8Array(state) : null, journal: scoped});
      } };
      stateReq.onsuccess = () => { state = stateReq.result || null; stateDone = true; finish(); };
      journalReq.onsuccess = () => { journal = journalReq.result || []; journalDone = true; finish(); };
      stateReq.onerror = journalReq.onerror = () => reject(tx.error || new Error('Could not read context journal'));
      tx.onabort = () => reject(tx.error || new Error('Context read transaction aborted'));
    });
  }

  async _appendJournal(record) {
    const db = await this._openDb();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(this.journalStore, 'readwrite');
      tx.objectStore(this.journalStore).add({...record, memoryKey: this.key});
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error || new Error('Could not append context journal'));
      tx.onabort = () => reject(tx.error || new Error('Context journal transaction aborted'));
    });
  }

  async _checkpoint(bytes) {
    const db = await this._openDb();
    const payload = bytes.slice().buffer;
    return new Promise((resolve, reject) => {
      const tx = db.transaction([this.stateStore, this.journalStore], 'readwrite');
      tx.objectStore(this.stateStore).put(payload, this.key);
      const store = tx.objectStore(this.journalStore);
      const keysReq = store.getAllKeys();
      const recordsReq = store.getAll();
      let keys = null, records = null;
      const prune = () => {
        if (!keys || !records) return;
        for (let i = 0; i < Math.min(keys.length, records.length); ++i) {
          if (records[i].memoryKey === this.key) store.delete(keys[i]);
        }
      };
      keysReq.onsuccess = () => { keys = keysReq.result || []; prune(); };
      recordsReq.onsuccess = () => { records = recordsReq.result || []; prune(); };
      keysReq.onerror = recordsReq.onerror = () => { try { tx.abort(); } catch {} };
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error || new Error('Could not checkpoint context'));
      tx.onabort = () => reject(tx.error || new Error('Context checkpoint transaction aborted'));
    });
  }

  // Call after the model loader confirms a successful model load.
  // Restore a checkpoint first, then replay only the appended records.
  async restoreAfterModelLoad() {
    const {state, journal} = await this._readStateAndJournal();
    this._fn('context_clear')();
    let restored = 0;
    if (state) restored = this._importSnapshot(state);
    for (const record of journal) {
      this._addToWasm(record.text, {kind: record.kind, importance: record.importance, timestamp: record.timestamp, sessionId: record.sessionId ?? 0n, projectId: record.projectId ?? 0n});
      restored++;
    }
    this.setScope();
    return {restored: restored > 0, count: this.count(), replayedJournalEntries: journal.length};
  }

  // Persist a record already inserted by the C++ agent report API. This avoids
  // inserting the same verified result into WASM RAM twice.
  async persistRecord(text, {kind = 4, importance = 190, timestamp = Date.now(), sessionId = this.sessionId, projectId = this.projectId} = {}) {
    const normalized = String(text);
    const bytes = new TextEncoder().encode(normalized);
    if (!bytes.length || bytes.length > this.maxInputBytes) throw new RangeError('Persisted memory record has invalid length');
    try {
      await this._appendJournal({text: normalized, kind: kind & 255, importance: importance & 255,
        timestamp: Math.trunc(timestamp), sessionId: String(sessionId), projectId: String(projectId)});
      this._lastError = null;
    } catch (err) { this._lastError = err; throw err; }
    return this.count();
  }

  // Compact the browser journal into one binary snapshot. Optional during normal use.
  async persistNow() {
    await this._checkpoint(this._exportSnapshot());
    this._lastError = null;
    return this.stats();
  }
  async flush() { return this.persistNow(); }

  async exportFile(filename = 'context_memory.ctx') {
    const bytes = this._exportSnapshot();
    const blob = new Blob([bytes], {type: 'application/octet-stream'});
    const url = URL.createObjectURL(blob);
    try {
      const a = document.createElement('a');
      a.href = url; a.download = filename; a.style.display = 'none';
      document.body.appendChild(a); a.click(); a.remove();
    } finally { setTimeout(() => URL.revokeObjectURL(url), 1000); }
    return bytes.byteLength;
  }

  async importFile(file) {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const count = this._importSnapshot(bytes);
    await this._checkpoint(bytes);
    return count;
  }
  lastPersistenceError() { return this._lastError; }
}
