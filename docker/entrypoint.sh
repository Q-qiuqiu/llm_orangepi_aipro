#!/usr/bin/env bash
set -euo pipefail

cd /app/qwen_server

if [ -f /usr/local/Ascend/ascend-toolkit/set_env.sh ]; then
  # Match the native Orange Pi Ascend runtime environment before starting ACL.
  # shellcheck disable=SC1091
  source /usr/local/Ascend/ascend-toolkit/set_env.sh
fi

export ASCEND_TOOLKIT_HOME="${ASCEND_TOOLKIT_HOME:-/usr/local/Ascend/ascend-toolkit/latest}"
export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-${ASCEND_TOOLKIT_HOME}}"
export ASCEND_AICPU_PATH="${ASCEND_AICPU_PATH:-${ASCEND_TOOLKIT_HOME}}"
export ASCEND_OPP_PATH="${ASCEND_OPP_PATH:-${ASCEND_TOOLKIT_HOME}/opp}"
export TOOLCHAIN_HOME="${TOOLCHAIN_HOME:-${ASCEND_TOOLKIT_HOME}/toolkit}"
export ASCEND_RT_VISIBLE_DEVICES="${ASCEND_RT_VISIBLE_DEVICES:-0}"

export LD_LIBRARY_PATH="/app/qwen_server/lib:${ASCEND_TOOLKIT_HOME}/tools/aml/lib64:${ASCEND_TOOLKIT_HOME}/tools/aml/lib64/plugin:${ASCEND_TOOLKIT_HOME}/lib64:${ASCEND_TOOLKIT_HOME}/lib64/plugin/opskernel:${ASCEND_TOOLKIT_HOME}/lib64/plugin/nnengine:${ASCEND_TOOLKIT_HOME}/opp/built-in/op_impl/ai_core/tbe/op_tiling/lib/linux/$(arch):/usr/local/Ascend/driver/lib64:/var/davinci/driver/lib64:/var/davinci/driver/lib64/common:/var/davinci/driver/lib64/driver:/usr/lib64:/lib64:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/app/qwen_server:/opt/qwen-venv/lib/python3.12/site-packages:/opt/qwen-venv/lib/python3.10/site-packages:${ASCEND_TOOLKIT_HOME}/python/site-packages:${ASCEND_TOOLKIT_HOME}/opp/built-in/op_impl/ai_core/tbe:${PYTHONPATH:-}"
export PATH="${ASCEND_TOOLKIT_HOME}/bin:${ASCEND_TOOLKIT_HOME}/compiler/ccec_compiler/bin:${ASCEND_TOOLKIT_HOME}/tools/ccec_compiler/bin:${PATH}"

: "${MODEL_DIR:=/models/DeepSeek-R1-Distill-Qwen-1.5B_server}"
: "${CONFIG_PATH:=${MODEL_DIR}/config/config.json}"
: "${TOKENIZER_DIR:=${MODEL_DIR}/tokenizer}"
: "${WEIGHT_DIR:=${MODEL_DIR}/converted}"
: "${HOST:=0.0.0.0}"
: "${PORT:=8081}"
: "${SERVED_MODEL_NAME:=deepseek-r1-distill-qwen-1.5b}"
: "${MAX_SEQ_LEN:=8192}"
: "${MAX_GEN_TOKEN:=8192}"
: "${TEMPERATURE:=0.6}"
: "${TOP_P:=0.9}"
: "${LOG_LEVEL:=info}"
: "${ROPE_IS_NEOX_STYLE:=true}"
: "${DEBUG_PRINT:=false}"
: "${USE_CHAT_TEMPLATE:=false}"
: "${QUANT_METHOD:=}"
: "${BIN:=/app/qwen_server/qwen_openai_server}"

if [ ! -x "${BIN}" ]; then
  echo "server binary not found: ${BIN}" >&2
  echo "build it on the host first: cmake -S . -B build_server -DCMAKE_BUILD_TYPE=Release && cmake --build build_server -j2" >&2
  exit 1
fi

if [ ! -f "${CONFIG_PATH}" ]; then
  echo "missing model config: ${CONFIG_PATH}" >&2
  exit 1
fi

if [ ! -d "${TOKENIZER_DIR}" ]; then
  echo "missing tokenizer dir: ${TOKENIZER_DIR}" >&2
  exit 1
fi

if [ ! -d "${WEIGHT_DIR}" ]; then
  echo "missing converted weight dir: ${WEIGHT_DIR}" >&2
  exit 1
fi

ARGS=(
  "${BIN}"
  "--config=${CONFIG_PATH}"
  "--tokenizer=${TOKENIZER_DIR}/"
  "--weight=${WEIGHT_DIR}"
  "--device_type=npu"
  "--host=${HOST}"
  "--port=${PORT}"
  "--served_model_name=${SERVED_MODEL_NAME}"
  "--max_seq_len=${MAX_SEQ_LEN}"
  "--max_gen_token=${MAX_GEN_TOKEN}"
  "--temperature=${TEMPERATURE}"
  "--top_p=${TOP_P}"
  "--debug_print=${DEBUG_PRINT}"
  "--log_level=${LOG_LEVEL}"
  "--rope_is_neox_style=${ROPE_IS_NEOX_STYLE}"
  "--use_chat_template=${USE_CHAT_TEMPLATE}"
)

if [ -n "${QUANT_METHOD}" ]; then
  ARGS+=("--quant_method=${QUANT_METHOD}")
fi

exec "${ARGS[@]}"
