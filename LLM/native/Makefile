# Bit-LLM v21 Artistic Intelligence Build
# Native: CPU-specific SIMD + OpenMP + LTO.
# WASM default: portable SIMD128 single-thread build.
# WASM parallel: optional OpenMP/Pthreads build; requires COOP/COEP headers.

.DEFAULT_GOAL := all
EMXX       ?= em++
HOSTCXX    ?= g++
NODE       ?= node
OUT_DIR    ?= ../js
BUILD_DIR  ?= build
CORE       ?= bit_llm_core.cpp

# CPU-first quality profile: more recurrent deliberation, wider search, and longer I/O.
# These are compile-time architecture settings; changing them intentionally invalidates old model files.
REASON_MAX_STEPS ?= 32
MEMORY_READ_BUDGET ?= 16
MEMORY_READ_TOP_K ?= 4
MAX_GEN_TOKENS ?= 512
MAX_BEAM ?= 16
MAX_Q ?= 256
MAX_A ?= 512
TOKEN_PICKER_CAP ?= 1024
BIT_LLM_DEFS ?=
BIT_LLM_DEFS += -DBIT_LLM_REASON_MAX_STEPS=$(REASON_MAX_STEPS) -DBIT_LLM_MEMORY_READ_BUDGET=$(MEMORY_READ_BUDGET) -DBIT_LLM_MEMORY_TOP_K=$(MEMORY_READ_TOP_K) -DBIT_LLM_MAX_GEN_TOKENS=$(MAX_GEN_TOKENS) -DBIT_LLM_MAX_BEAM=$(MAX_BEAM) -DBIT_LLM_MAX_Q=$(MAX_Q) -DBIT_LLM_MAX_A=$(MAX_A) -DBIT_LLM_TOKEN_PICKER_CAP=$(TOKEN_PICKER_CAP)

# Maximum local-machine codegen. Use NATIVE_ARCH=x86-64-v3 for a portable AVX2 build.
NATIVE_ARCH ?= native
NATIVE_CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -fno-omit-frame-pointer -march=$(NATIVE_ARCH) -mtune=$(NATIVE_ARCH) -fopenmp -flto=auto -fexceptions -fno-rtti -fno-math-errno
NATIVE_CXXFLAGS += $(BIT_LLM_DEFS)

# Keep the browser-compatible build independent of SharedArrayBuffer.
WASM_CXXFLAGS ?= -O3 -flto -DNDEBUG -std=c++20 -msimd128 -mbulk-memory -fexceptions -fno-rtti -fno-math-errno
WASM_CXXFLAGS += $(BIT_LLM_DEFS)
WASM_LDFLAGS ?= -O3 -flto --no-entry \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createBitLLMModule \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ABORTING_MALLOC=0 \
  -s INITIAL_MEMORY=268435456 \
  -s STACK_SIZE=4194304 \
  -s EXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
  -s EXPORTED_FUNCTIONS='$(WASM_EXPORTS_JSON)' \
  -s ENVIRONMENT='web,worker,node'
TRAIN_LDFLAGS ?= $(subst createBitLLMModule,createBitLLMTrainModule,$(WASM_LDFLAGS))

# Optional WebAssembly threads. Keep this a separate artifact because pthread and
# non-pthread Wasm modules are not interchangeable at runtime.
WASM_THREAD_COUNT ?= 4
WASM_MT_CXXFLAGS ?= $(WASM_CXXFLAGS) -fopenmp -pthread
WASM_MT_LDFLAGS ?= $(subst createBitLLMModule,createBitLLMParallelModule,$(WASM_LDFLAGS)) -fopenmp -pthread -sPTHREAD_POOL_SIZE=$(WASM_THREAD_COUNT) -sPTHREAD_POOL_SIZE_STRICT=2
TRAIN_MT_LDFLAGS ?= $(subst createBitLLMParallelModule,createBitLLMTrainParallelModule,$(WASM_MT_LDFLAGS))

WASM_JS    := $(OUT_DIR)/bit_llm.js
WASM_FILE  := $(OUT_DIR)/bit_llm.wasm
WASM_OBJ   := $(BUILD_DIR)/bit_llm.wasm.o
WASM_DEP   := $(WASM_OBJ:.o=.d)
TRAIN_JS   := $(OUT_DIR)/bit_llm_train.js
TRAIN_FILE := $(OUT_DIR)/bit_llm_train.wasm
TRAIN_OBJ  := $(BUILD_DIR)/bit_llm.train.wasm.o
TRAIN_DEP  := $(TRAIN_OBJ:.o=.d)

WASM_MT_JS   := $(OUT_DIR)/bit_llm_mt.js
WASM_MT_FILE := $(OUT_DIR)/bit_llm_mt.wasm
WASM_MT_OBJ  := $(BUILD_DIR)/bit_llm.mt.wasm.o
TRAIN_MT_JS  := $(OUT_DIR)/bit_llm_train_mt.js
TRAIN_MT_FILE:= $(OUT_DIR)/bit_llm_train_mt.wasm
TRAIN_MT_OBJ := $(BUILD_DIR)/bit_llm.train.mt.wasm.o

# Full v20 ABI, including Context Memory, Video Agent plan, trained-router telemetry, and hierarchical output metrics.
WASM_EXPORTS := _wasm_bit_llm_init _wasm_bit_llm_get_input_ptr _wasm_bit_llm_get_output_ptr _wasm_bit_llm_infer _wasm_bit_llm_get_data_ptr _wasm_bit_llm_get_data_size _wasm_bit_llm_load_dataset _wasm_bit_llm_get_ds_stat _wasm_bit_llm_train_epoch _wasm_bit_llm_get_stat _wasm_bit_llm_weight_density _wasm_bit_llm_get_model_ptr _wasm_bit_llm_get_model_size _wasm_bit_llm_validate_model _wasm_bit_llm_get_token_ptr _wasm_bit_llm_get_token_len _wasm_bit_llm_get_vocab_count _wasm_bit_llm_get_head_words _wasm_bit_llm_get_trace_ptr _wasm_bit_llm_get_trace_count _wasm_bit_llm_set_beam _wasm_bit_llm_get_beam _wasm_bit_llm_get_chunk_buffer_ptr _wasm_bit_llm_load_chunk_direct _wasm_bit_llm_finalize_load _wasm_bit_llm_think_begin _wasm_bit_llm_think_step _wasm_bit_llm_think_leader_votes _wasm_bit_llm_think_views _wasm_bit_llm_think_cache_hits _wasm_bit_llm_think_cache_misses _wasm_bit_llm_get_output_tokens_scanned _wasm_bit_llm_get_output_head_macs_low _wasm_bit_llm_get_output_head_macs_high _wasm_bit_llm_get_output_router_macs_low _wasm_bit_llm_get_output_router_macs_high _wasm_bit_llm_get_output_clusters_visited _wasm_bit_llm_get_router_cluster_count _wasm_bit_llm_reset_perf_counters _wasm_bit_llm_think_finish _wasm_bit_llm_begin_load _wasm_bit_llm_get_header_size _wasm_bit_llm_get_proj_entry_size _wasm_bit_llm_get_proj_bytes _wasm_bit_llm_get_chunk_buffer_size _wasm_bit_llm_get_ram_bytes _wasm_bit_llm_load_dataset_inc _wasm_bit_llm_begin_session _wasm_bit_llm_end_session _wasm_bit_llm_get_temp _wasm_bit_llm_set_temp _wasm_bit_llm_get_creativity _wasm_bit_llm_set_creativity _wasm_bit_llm_set_search_seed _wasm_bit_llm_get_novelty _wasm_bit_llm_set_min_lp _wasm_bit_llm_get_answer_source _wasm_bit_llm_export_begin _wasm_bit_llm_export_read _wasm_bit_llm_think_confident _wasm_bit_llm_get_hyper _wasm_bit_llm_set_hyper _wasm_bit_llm_set_train_opts _wasm_bit_llm_eval_range _wasm_bit_llm_eval_holdout _wasm_bit_llm_get_eval_nll _wasm_bit_llm_get_holdout_count _wasm_bit_llm_set_holdout _wasm_bit_llm_set_aug _wasm_bit_llm_is_holdout_line _wasm_bit_llm_snapshot_best _wasm_bit_llm_restore_best _wasm_bit_llm_calibrate_temperature _wasm_bit_llm_setoption _wasm_bit_llm_list_options _wasm_bit_llm_feedback _wasm_bit_llm_feedback_text _wasm_bit_llm_explain _wasm_bit_llm_selftest_acc _wasm_bit_llm_selftest_simd _wasm_bit_llm_selftest_text _wasm_bit_llm_bench_output _wasm_bit_llm_reasoning_engine_version _wasm_bit_llm_get_reasoning_steps _wasm_bit_llm_get_reasoning_active _wasm_bit_llm_get_reasoning_max_steps _wasm_bit_llm_set_reasoning_budget _wasm_bit_llm_get_reasoning_last_steps _wasm_bit_llm_get_reasoning_halt_permille _wasm_bit_llm_get_memory_reads _wasm_bit_llm_get_memory_read_probability_permille _wasm_bit_llm_get_memory_read_budget _wasm_bit_llm_context_add _wasm_bit_llm_context_search _wasm_bit_llm_context_clear _wasm_bit_llm_context_set_scope _wasm_bit_llm_context_count _wasm_bit_llm_context_indexed_terms _wasm_bit_llm_context_ram_bytes _wasm_bit_llm_context_hit_id_lo _wasm_bit_llm_context_hit_id_hi _wasm_bit_llm_context_hit_score _wasm_bit_llm_context_hit_text_len _wasm_bit_llm_context_hit_text_ptr _wasm_bit_llm_context_hit_kind _wasm_bit_llm_context_export_begin _wasm_bit_llm_context_export_read _wasm_bit_llm_context_import_begin _wasm_bit_llm_context_import_chunk _wasm_bit_llm_context_import_finalize _wasm_bit_llm_agent_plan_set_action_catalog _wasm_bit_llm_agent_plan_begin _wasm_bit_llm_agent_plan_build_prompt _wasm_bit_llm_agent_plan_parse_output _wasm_bit_llm_agent_plan_add_step _wasm_bit_llm_agent_plan_revise_step _wasm_bit_llm_agent_plan_claim_next _wasm_bit_llm_agent_plan_action_ptr _wasm_bit_llm_agent_plan_payload_ptr _wasm_bit_llm_agent_plan_postcondition_ptr _wasm_bit_llm_agent_plan_active_revision _wasm_bit_llm_agent_plan_active_attempt _wasm_bit_llm_agent_plan_report _wasm_bit_llm_agent_plan_build_repair_prompt _wasm_bit_llm_agent_plan_parse_repair _wasm_bit_llm_agent_plan_status _wasm_bit_llm_agent_plan_step_count _wasm_bit_llm_agent_plan_ready_count _wasm_bit_llm_agent_plan_complete _wasm_bit_llm_agent_plan_terminal_failure _wasm_bit_llm_agent_plan_error_ptr _wasm_bit_llm_artistic_version _wasm_bit_llm_artistic_intent_dims _wasm_bit_llm_artistic_feature_dims _wasm_bit_llm_artistic_candidate_capacity _wasm_bit_llm_artistic_strategy_count _wasm_bit_llm_artistic_set_intent_text _wasm_bit_llm_artistic_get_intent_ptr _wasm_bit_llm_artistic_intent_buffer_ptr _wasm_bit_llm_artistic_set_intent_vector _wasm_bit_llm_artistic_candidate_buffer_ptr _wasm_bit_llm_artistic_clear_candidates _wasm_bit_llm_artistic_reset_preferences _wasm_bit_llm_artistic_candidate_submit _wasm_bit_llm_artistic_candidate_score _wasm_bit_llm_artistic_rank _wasm_bit_llm_artistic_preference _wasm_bit_llm_artistic_preference_updates _wasm_bit_llm_artistic_select_strategy _wasm_bit_llm_artistic_get_strategy_json _wasm_bit_llm_artistic_get_score_json _wasm_bit_llm_artistic_state_size _wasm_bit_llm_artistic_state_out_ptr _wasm_bit_llm_artistic_state_in_ptr _wasm_bit_llm_artistic_export_state _wasm_bit_llm_artistic_import_state _wasm_bit_llm_get_block_stat _wasm_bit_llm_get_nodes _wasm_bit_llm_think_nodes _wasm_bit_llm_set_desc _wasm_bit_llm_set_sampling _wasm_bit_llm_get_top_p _wasm_bit_llm_block_in_ram _wasm_bit_llm_get_file_size _wasm_bit_llm_get_proj_base _wasm_bit_llm_get_stream_bytes _wasm_bit_llm_get_train_diag _wasm_bit_llm_tune_hyper _wasm_bit_llm_set_ram_budget_kb _wasm_bit_llm_is_paged _wasm_bit_llm_get_block_tokens _wasm_bit_llm_get_max_slots
comma := ,
empty :=
space := $(empty) $(empty)
WASM_EXPORTS_JSON = [$(subst $(space),$(comma),$(foreach f,$(WASM_EXPORTS),"$(f)"))]

.PHONY: all all-mt wasm train-wasm wasm-mt train-wasm-mt native-check native-train-check verify test bench-v20 clean help check-emscripten native-quality native-profile scale-test
all: wasm train-wasm
all-mt: wasm-mt train-wasm-mt

help:
	@echo "Bit-LLM v21 artistic-intelligence targets:"
	@echo "  make clean && make -j2 all  Build portable SIMD128 inference + trainer Wasm"
	@echo "  make -j2 all-mt             Build separate OpenMP/Pthreads Wasm artifacts"
	@echo "  make verify              Compile native inference + trainer for this CPU"
	@echo "  make test                Run ContextMemory, Video Agent, artistic critic, and rearchitecture tests"
	@echo "  make bench-v20           Benchmark hierarchical router and fine-head work counters"
	@echo "  NATIVE_ARCH=native       Tune native build for this exact CPU (default)"
	@echo "  NATIVE_ARCH=x86-64-v3    Build portable AVX2-class native code"
	@echo "  CPU-first quality defaults: reasoning=32, beam=16, output=256, query=160"
	@echo "  Override: make verify REASON_MAX_STEPS=24 MAX_BEAM=8 ..."
	@echo "  WASM_THREAD_COUNT=4      Number of pre-created Wasm pthread workers"
	@echo "  CORE=...                 Select another core source"

check-emscripten:
	@command -v $(EMXX) >/dev/null 2>&1 || { echo "ERROR: cannot find $(EMXX); activate/run the Emscripten SDK"; exit 127; }
$(BUILD_DIR):
	mkdir -p $@
$(OUT_DIR):
	mkdir -p $@

CORE_DEPS := $(CORE) context_memory.hpp video_agent_runtime.hpp artistic_intelligence.hpp Makefile

$(WASM_OBJ): $(CORE_DEPS) | $(BUILD_DIR) check-emscripten
	$(EMXX) $(WASM_CXXFLAGS) -MMD -MP -c $(CORE) -o $@
$(WASM_JS): $(WASM_OBJ) Makefile | $(OUT_DIR) check-emscripten
	$(EMXX) $(WASM_CXXFLAGS) $(WASM_OBJ) $(WASM_LDFLAGS) -o $@
	@test -s $(WASM_JS) && test -s $(WASM_FILE)
wasm: $(WASM_JS)

$(TRAIN_OBJ): $(CORE_DEPS) | $(BUILD_DIR) check-emscripten
	$(EMXX) $(WASM_CXXFLAGS) -DBIT_LLM_TRAIN -MMD -MP -c $(CORE) -o $@
$(TRAIN_JS): $(TRAIN_OBJ) Makefile | $(OUT_DIR) check-emscripten
	$(EMXX) $(WASM_CXXFLAGS) -DBIT_LLM_TRAIN $(TRAIN_OBJ) $(TRAIN_LDFLAGS) -o $@
	@test -s $(TRAIN_JS) && test -s $(TRAIN_FILE)
train-wasm: $(TRAIN_JS)

$(WASM_MT_OBJ): $(CORE_DEPS) | $(BUILD_DIR) check-emscripten
	$(EMXX) $(WASM_MT_CXXFLAGS) -MMD -MP -c $(CORE) -o $@
$(WASM_MT_JS): $(WASM_MT_OBJ) Makefile | $(OUT_DIR) check-emscripten
	$(EMXX) $(WASM_MT_CXXFLAGS) $(WASM_MT_OBJ) $(WASM_MT_LDFLAGS) -o $@
	@test -s $(WASM_MT_JS) && test -s $(WASM_MT_FILE)
wasm-mt: $(WASM_MT_JS)

$(TRAIN_MT_OBJ): $(CORE_DEPS) | $(BUILD_DIR) check-emscripten
	$(EMXX) $(WASM_MT_CXXFLAGS) -DBIT_LLM_TRAIN -MMD -MP -c $(CORE) -o $@
$(TRAIN_MT_JS): $(TRAIN_MT_OBJ) Makefile | $(OUT_DIR) check-emscripten
	$(EMXX) $(WASM_MT_CXXFLAGS) -DBIT_LLM_TRAIN $(TRAIN_MT_OBJ) $(TRAIN_MT_LDFLAGS) -o $@
	@test -s $(TRAIN_MT_JS) && test -s $(TRAIN_MT_FILE)
train-wasm-mt: $(TRAIN_MT_JS)

native-check: | $(BUILD_DIR)
	$(HOSTCXX) $(NATIVE_CXXFLAGS) -Wall -Wextra -c $(CORE) -o $(BUILD_DIR)/bit_llm.native.o
native-train-check: | $(BUILD_DIR)
	$(HOSTCXX) $(NATIVE_CXXFLAGS) -DBIT_LLM_TRAIN -Wall -Wextra -c $(CORE) -o $(BUILD_DIR)/bit_llm.train.native.o
verify: native-check native-train-check

# Recommended native profile. Respect explicit OMP_NUM_THREADS supplied by caller;
# otherwise let OpenMP use the machine's available logical CPUs.
native-quality: verify scale-test

scale-test: | $(BUILD_DIR)
	$(HOSTCXX) -O2 -std=c++20 -Wall -Wextra -I. scale_test.cpp -o $(BUILD_DIR)/scale_test
	$(BUILD_DIR)/scale_test

native-profile: | $(BUILD_DIR)
	@echo "Compiler: $$($(HOSTCXX) --version | head -1)"
	@echo "CPU: $$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | xargs)"
	@echo "Logical CPUs: $$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
	@echo "OpenMP threads: $${OMP_NUM_THREADS:-runtime default}"
	@echo "Build profile: native=$(NATIVE_ARCH), reasoning=$(REASON_MAX_STEPS), beam=$(MAX_BEAM), max_output=$(MAX_GEN_TOKENS), max_query=$(MAX_Q), max_answer=$(MAX_A), six_pillar_scale=enabled"

test:
	CXX=$(HOSTCXX) ./test_native.sh

BENCH_VOCAB ?= 16384
BENCH_TOP_K ?= 8
BENCH_REPEATS ?= 300
BENCH_CXXFLAGS ?= -O2 -DNDEBUG -std=c++20 -march=$(NATIVE_ARCH) -mtune=$(NATIVE_ARCH) -fopenmp -fexceptions -fno-rtti -fno-math-errno $(BIT_LLM_DEFS)
bench-v20: | $(BUILD_DIR)
	$(HOSTCXX) $(BENCH_CXXFLAGS) -I. bench_hierarchical.cpp -o $(BUILD_DIR)/bench_v20
	OMP_NUM_THREADS=$${OMP_NUM_THREADS:-4} $(BUILD_DIR)/bench_v20 $(BENCH_VOCAB) $(BENCH_TOP_K) $(BENCH_REPEATS)

clean:
	rm -rf $(BUILD_DIR) build-native-tests
	rm -f $(WASM_JS) $(WASM_FILE) $(TRAIN_JS) $(TRAIN_FILE) $(WASM_MT_JS) $(WASM_MT_FILE) $(TRAIN_MT_JS) $(TRAIN_MT_FILE)

-include $(WASM_DEP) $(TRAIN_DEP)
