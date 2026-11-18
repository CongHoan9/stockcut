/**
 * Ready-to-wire adapter for the Bit-LLM Video Agent.
 * Instantiate only after the Emscripten Module and model are ready.
 * The host must implement validateAction() against the actual editor/project state.
 */
import { runAgentTurn } from './agent-runtime.js';
import { BitLLMContextMemory } from '../js/context-memory-bridge.js';

export async function createVideoAgent({
  Module,
  editor,
  memoryOptions = {},
  rememberByDefault = true,
} = {}) {
  if (!Module || typeof Module._wasm_bit_llm_infer !== 'function') {
    throw new TypeError('Module must be initialized and expose wasm_bit_llm_infer');
  }
  if (!Module.HEAPU8) throw new TypeError('Module.HEAPU8 is unavailable; check EXPORTED_RUNTIME_METHODS in Makefile');
  const memory = new BitLLMContextMemory(Module, memoryOptions);
  const restore = await memory.restoreAfterModelLoad();

  const validateAction = async (payload, context) => {
    if (!editor || typeof editor.validateAction !== 'function') {
      return { valid: false, reason: 'Editor adapter must validate actions against the current project before execution.' };
    }
    return await editor.validateAction(payload, context);
  };
  const executeAction = async payload => {
    if (!editor || typeof editor.executeAction !== 'function') {
      throw new Error('Editor adapter does not implement executeAction');
    }
    return await editor.executeAction(payload);
  };

  return Object.freeze({
    memory,
    restore,
    async turn(userText, options = {}) {
      return runAgentTurn(Module, userText, {
        ...options,
        contextMemory: memory,
        validateAction,
        executeAction,
        remember: options.remember ?? rememberByDefault,
      });
    },
  });
}
