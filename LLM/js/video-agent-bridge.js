/*
 * Bit-LLM v18 video-agent host bridge.
 *
 * The model proposes a plan. This bridge validates the plan, asks the host editor
 * adapter to execute each allow-listed action, and requires a separate read-back
 * verifier before the C++ state machine marks a step complete.
 *
 * Example:
 * const agent = new BitLLMVideoAgent(Module, {
 *   actions: ['add_item', 'add_effect', 'set_config'],
 *   contextMemory, // BitLLMContextMemory; recommended for durable verified-action memory
 *   executeAction: async ({action, params}) => editor.dispatch(action, params),
 *   verifyAction: async ({action, params, postcondition, result}) => {
 *     const state = await editor.readState();
 *     const ok = verifyPostcondition(state, action, params, postcondition, result);
 *     return {ok, evidence: ok ? JSON.stringify(extractRelevantState(state)) : 'postcondition not observed'};
 *   }
 * });
 * const result = await agent.run('Thêm ảnh bìa, sau đó áp dụng hiệu ứng bloom');
 */

export class BitLLMVideoAgent {
  constructor(Module, options = {}) {
    if (!Module) throw new TypeError('A loaded Bit-LLM WASM Module is required');
    if (typeof options.executeAction !== 'function' || typeof options.verifyAction !== 'function') {
      throw new TypeError('executeAction and verifyAction callbacks are required');
    }
    this.Module = Module;
    this.actions = Array.from(new Set((options.actions || []).map(String)));
    if (!this.actions.length) throw new TypeError('Register at least one host-supported editor action');
    this.executeAction = options.executeAction;
    this.verifyAction = options.verifyAction;
    this.contextMemory = options.contextMemory || null;
    this.autoRestoreMemory = options.autoRestoreMemory !== false;
    this.memoryReady = options.memoryReady === true;
    this.sessionId = 0n; this.projectId = 0n;
    this._runCounter = 0; this.runId = '';
    this.maxRetries = Math.max(0, Math.min(4, options.maxRetries ?? 2));
    this.maxPlanRepairs = Math.max(0, Math.min(2, options.maxPlanRepairs ?? 1));
    this.maxOutputBytes = Math.max(1024, Math.min(8191, options.maxOutputBytes ?? 8191));
    this._encoder = new TextEncoder();
    this._decoder = new TextDecoder();
  }

  async _ensureMemoryReady() {
    if (this.memoryReady || !this.contextMemory || !this.autoRestoreMemory) { this.memoryReady = true; return; }
    if (typeof this.contextMemory.restoreAfterModelLoad === 'function') {
      await this.contextMemory.restoreAfterModelLoad();
    }
    this.memoryReady = true;
  }
  _fn(name) {
    const fn = this.Module[`_wasm_bit_llm_${name}`];
    if (typeof fn !== 'function') throw new Error(`Missing WASM export wasm_bit_llm_${name}; rebuild using the v18 Makefile`);
    return fn;
  }
  _heap() {
    if (!this.Module.HEAPU8) throw new Error('WASM HEAPU8 is unavailable');
    return this.Module.HEAPU8;
  }
  _writeInput(text) {
    const bytes = this._encoder.encode(String(text));
    if (bytes.length > this.maxOutputBytes) throw new RangeError(`WASM input is ${bytes.length} bytes, limit ${this.maxOutputBytes}`);
    const ptr = this._fn('get_input_ptr')();
    this._heap().set(bytes, ptr);
    this._heap()[ptr + bytes.length] = 0;
    return bytes.length;
  }
  _readCString(ptr) {
    const heap = this._heap();
    let end = ptr;
    const limit = Math.min(heap.length, ptr + 8192);
    while (end < limit && heap[end] !== 0) ++end;
    return this._decoder.decode(heap.subarray(ptr, end));
  }
  _readOutput(len) {
    if (!Number.isInteger(len) || len < 0 || len > this.maxOutputBytes) throw new Error(`Invalid inference output length ${len}`);
    const ptr = this._fn('get_output_ptr')();
    return this._decoder.decode(this._heap().subarray(ptr, ptr + len));
  }
  _infer(prompt) {
    const len = this._writeInput(prompt);
    const outLen = this._fn('infer')(len);
    if (!outLen) throw new Error('Bit-LLM returned an empty response; check model load and retraining');
    return this._readOutput(outLen);
  }
  _setScope(sessionId = 0n, projectId = 0n) {
    const toU64 = value => {
      const n = BigInt(value);
      if (n < 0n || n > 0xffffffffffffffffn) throw new RangeError('Scope IDs must be unsigned 64-bit integers');
      return n;
    };
    const s = toU64(sessionId), p = toU64(projectId);
    this.sessionId = s; this.projectId = p;
    this._fn('context_set_scope')(Number(s & 0xffffffffn), Number(s >> 32n), Number(p & 0xffffffffn), Number(p >> 32n));
    if (this.contextMemory && typeof this.contextMemory.setScope === 'function') this.contextMemory.setScope(s, p);
  }
  _extractJsonObject(raw) {
    const start = raw.indexOf('{');
    if (start < 0) throw new Error(`Model did not produce a JSON object: ${raw.slice(0, 220)}`);
    let depth = 0, quoted = false, escaped = false;
    for (let i = start; i < raw.length; ++i) {
      const c = raw[i];
      if (quoted) {
        if (escaped) escaped = false;
        else if (c === '\\') escaped = true;
        else if (c === '"') quoted = false;
        continue;
      }
      if (c === '"') quoted = true;
      else if (c === '{') ++depth;
      else if (c === '}' && --depth === 0) {
        const text = raw.slice(start, i + 1);
        try { return JSON.parse(text); }
        catch (err) { throw new Error(`Generated JSON is invalid: ${err.message}. Output: ${text.slice(0, 300)}`); }
      }
    }
    throw new Error('Generated JSON object is incomplete or has unbalanced braces');
  }
  _validatePlan(doc) {
    if (!doc || typeof doc !== 'object' || !Array.isArray(doc.steps) || doc.steps.length < 1 || doc.steps.length > 48) {
      throw new Error('Plan must be {"steps":[...]} with 1..48 steps');
    }
    const known = new Set();
    const allowed = new Set(this.actions);
    return doc.steps.map((step, index) => {
      if (!step || typeof step !== 'object' || Array.isArray(step)) throw new Error(`Step ${index + 1} is not an object`);
      const id = Number(step.id);
      if (!Number.isInteger(id) || id <= 0 || id > 0xffffffff || known.has(id)) throw new Error(`Step ${index + 1} has an invalid or duplicate id`);
      if (typeof step.action !== 'string' || !allowed.has(step.action)) throw new Error(`Step ${id} uses an unregistered action`);
      if (!step.params || typeof step.params !== 'object' || Array.isArray(step.params)) throw new Error(`Step ${id} params must be a JSON object`);
      if (typeof step.verify !== 'string' || !step.verify.trim() || this._encoder.encode(step.verify).length > 512) throw new Error(`Step ${id} needs a concise observable verify condition`);
      const deps = step.depends_on ?? [];
      if (!Array.isArray(deps) || deps.length > 12) throw new Error(`Step ${id} dependencies must be an array of at most 12 IDs`);
      for (const dep of deps) if (!Number.isInteger(dep) || !known.has(dep)) throw new Error(`Step ${id} must depend only on earlier steps`);
      known.add(id);
      const payload = JSON.stringify(step.params);
      if (this._encoder.encode(payload).length > 4095) throw new Error(`Step ${id} parameters exceed the payload limit`);
      return {id, action: step.action, depends_on: deps, params: step.params, verify: step.verify};
    });
  }
  _addStep(step) {
    const packed = `${step.action}\n${JSON.stringify(step.params)}\n${step.verify}\n${step.depends_on.length ? step.depends_on.join(',') : '-'}`;
    const len = this._writeInput(packed);
    if (!this._fn('agent_plan_add_step')(step.id, this.maxRetries, len)) {
      throw new Error(`C++ rejected plan step ${step.id}: ${this._readCString(this._fn('agent_plan_error_ptr')())}`);
    }
  }
  async createPlan(task, {sessionId = 0n, projectId = 0n} = {}) {
    await this._ensureMemoryReady();
    if (typeof task !== 'string' || !task.trim()) throw new TypeError('A non-empty video-editing task is required');
    this._setScope(sessionId, projectId);
    let len = this._writeInput(this.actions.join(','));
    if (!this._fn('agent_plan_set_action_catalog')(len)) throw new Error('C++ rejected the host action catalog');
    len = this._writeInput(task);
    if (!this._fn('agent_plan_begin')(len)) throw new Error('C++ rejected the task text');
    const promptLen = this._fn('agent_plan_build_prompt')();
    if (!promptLen) throw new Error('C++ could not build a planning prompt');
    const prompt = this._readCString(this._fn('get_input_ptr')());
    let raw = this._infer(prompt), steps = null, lastError = null;
    for (let attempt = 0; attempt <= this.maxPlanRepairs; ++attempt) {
      try {
        steps = this._validatePlan(this._extractJsonObject(raw));
        break;
      } catch (err) {
        lastError = err instanceof Error ? err.message : String(err);
        if (attempt >= this.maxPlanRepairs) break;
        const repairPrompt = `VIDEO_PLAN_FORMAT_REPAIR output only valid JSON matching {\"steps\":[{\"id\":1,\"action\":\"registered_action\",\"depends_on\":[],\"params\":{},\"verify\":\"observable editor state\"}]} FIX_ERROR ${lastError} TASK ${task} SUPPORTED_ACTIONS ${this.actions.join(' ')} PREVIOUS_OUTPUT ${raw}`;
        raw = this._infer(repairPrompt);
      }
    }
    if (!steps) throw new Error(`Could not produce a valid, allow-listed video plan: ${lastError}`);
    for (const step of steps) this._addStep(step);
    if (this._fn('agent_plan_step_count')() !== steps.length) throw new Error('Not all generated steps were committed to the C++ plan');
    return {task, steps, raw};
  }
  _activeStep() {
    const id = this._fn('agent_plan_claim_next')();
    if (!id) return null;
    const action = this._readCString(this._fn('agent_plan_action_ptr')());
    const paramsText = this._readCString(this._fn('agent_plan_payload_ptr')());
    const verify = this._readCString(this._fn('agent_plan_postcondition_ptr')());
    let params;
    try { params = JSON.parse(paramsText); }
    catch { throw new Error(`C++ plan contains invalid JSON params for step ${id}`); }
    return {id, action, params, verify, revision: this._fn('agent_plan_active_revision')(), attempt: this._fn('agent_plan_active_attempt')()};
  }
  _report(step, actionSucceeded, verified, evidenceOrError) {
    const text = String(evidenceOrError || (verified ? 'verified' : 'verification failed'));
    const len = this._writeInput(text);
    if (!this._fn('agent_plan_report')(step.id, actionSucceeded ? 1 : 0, verified ? 1 : 0, len)) {
      throw new Error(`C++ could not record result for step ${step.id}`);
    }
  }
  async _repair(stepId) {
    const promptLen = this._fn('agent_plan_build_repair_prompt')(stepId);
    if (!promptLen) throw new Error(`No repair prompt available for step ${stepId}`);
    const prompt = this._readCString(this._fn('get_input_ptr')());
    let raw = this._infer(prompt), doc = null, lastError = null;
    for (let attempt = 0; attempt <= this.maxPlanRepairs; ++attempt) {
      try {
        const candidate = this._extractJsonObject(raw);
        if (Number(candidate.id) !== stepId || typeof candidate.action !== 'string' || !this.actions.includes(candidate.action) ||
            !candidate.params || typeof candidate.params !== 'object' || Array.isArray(candidate.params) || typeof candidate.verify !== 'string' || !candidate.verify.trim()) {
          throw new Error('repair response violates id/action/params/verify schema');
        }
        doc = candidate; break;
      } catch (err) {
        lastError = err instanceof Error ? err.message : String(err);
        if (attempt >= this.maxPlanRepairs) break;
        raw = this._infer(`VIDEO_REPAIR_FORMAT_FIX output only valid JSON object with id=${stepId}, action, params object, verify string. FIX_ERROR ${lastError}. ORIGINAL_REPAIR ${raw}. SUPPORTED_ACTIONS ${this.actions.join(' ')}`);
      }
    }
    if (!doc) throw new Error(`Repair response for step ${stepId} remains invalid: ${lastError}`);
    const payload = JSON.stringify(doc.params);
    const packed = `${doc.action}\n${payload}\n${doc.verify}`;
    const len = this._writeInput(packed);
    if (!this._fn('agent_plan_revise_step')(stepId, len)) {
      throw new Error(`C++ rejected repair for step ${stepId}: ${this._readCString(this._fn('agent_plan_error_ptr')())}`);
    }
    return {raw, action: doc.action, params: doc.params, verify: doc.verify};
  }
  async run(task, options = {}) {
    this.runId = `${Date.now().toString(36)}-${(++this._runCounter).toString(36)}`;
    const plan = await this.createPlan(task, options);
    const executed = [];
    const hardLimit = plan.steps.length * (this.maxRetries + 1) + 1;
    for (let turn = 0; turn < hardLimit; ++turn) {
      const step = this._activeStep();
      if (!step) {
        if (this._fn('agent_plan_complete')()) return {ok: true, plan, executed};
        if (this._fn('agent_plan_terminal_failure')()) throw new Error('Video plan entered a terminal failure/blocked state');
        throw new Error('Video plan has unfinished steps but none are runnable; inspect dependency graph and step statuses');
      }
      let result = null, verification = null;
      try {
        const idempotencyKey = `${this.runId}:${step.id}:${step.revision}`;
        result = await this.executeAction({action: step.action, params: step.params, stepId: step.id,
          runId: this.runId, idempotencyKey, attempt: step.attempt, revision: step.revision,
          postcondition: step.verify, task});
        const actionOk = result !== null && result !== undefined && result.ok !== false;
        if (!actionOk) throw new Error(result?.error || 'Host editor adapter reported action failure');
        verification = await this.verifyAction({action: step.action, params: step.params, postcondition: step.verify,
          result, stepId: step.id, runId: this.runId, attempt: step.attempt, revision: step.revision, task});
        if (!verification || verification.ok !== true || typeof verification.evidence !== 'string' || !verification.evidence.trim()) {
          throw new Error(verification?.error || 'Verifier must return {ok:true,evidence:<editor state read-back>}');
        }
      } catch (err) {
        const errorText = err instanceof Error ? err.message : String(err);
        this._report(step, false, false, errorText);
        const status = this._fn('agent_plan_status')(step.id);
        if (status === 3) {
          const repair = await this._repair(step.id);
          executed.push({step, failed: errorText, repaired: repair});
          continue;
        }
        throw new Error(`Step ${step.id} failed after ${step.attempt} attempt(s): ${errorText}`);
      }

      // Commit verified success before attempting optional browser-journal persistence.
      this._report(step, true, true, verification.evidence);
      const completed = {step, result, evidence: verification.evidence};
      executed.push(completed);
      if (this.contextMemory && typeof this.contextMemory.persistRecord === 'function') {
        try {
          await this.contextMemory.persistRecord(`Verified video action: step=${step.id}; action=${step.action}; evidence=${verification.evidence}`,
            {kind: 4, importance: 190, sessionId: this.sessionId, projectId: this.projectId});
        } catch (err) {
          completed.memoryPersistenceError = err instanceof Error ? err.message : String(err);
        }
      }
    }
    throw new Error('Plan execution exceeded the retry safety limit');
  }
}
