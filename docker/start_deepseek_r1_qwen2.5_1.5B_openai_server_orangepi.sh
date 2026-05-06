#!/usr/bin/env bash
set -e

BIN="./qwen_openai_server"
if [ ! -x "${BIN}" ]; then
  echo "server binary not found: ${BIN}"
  echo "build it first: cmake -S . -B build_server && cmake --build build_server -j2"
  exit 1
fi

MODEL_DIR="/root/models/DeepSeek-R1-Distill-Qwen-1.5B_server"

"${BIN}" \
--config="${MODEL_DIR}/config/config.json" \
--tokenizer="${MODEL_DIR}/tokenizer/" \
--weight="${MODEL_DIR}/converted" \
--device_type=npu \
--host=0.0.0.0 \
--port=8081 \
--served_model_name=deepseek-r1-distill-qwen-1.5b \
--max_seq_len=8192 \
--max_gen_token=8192 \
--temperature=0.6 \
--top_p=0.9 \
--debug_print=false \
--log_level=info \
--rope_is_neox_style=true
