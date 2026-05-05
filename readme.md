# Qwen OpenAI Server for Orange Pi AIPro

本仓库只保留 Qwen2/DeepSeek-R1-Distill-Qwen 在 Orange Pi AIPro NPU 上的 HTTP 推理服务。

服务启动后会先加载模型，然后常驻等待 HTTP 请求，接口兼容常用 OpenAI 格式：

- `GET /health`
- `GET /v1/models`
- `POST /v1/chat/completions`
- `POST /v1/completions`

推理日志会输出：

- 用户 prompt
- 模型响应
- 推理速度：`total_tps`、`decode_tps`、`ttft`、`decode`

## 目录说明

```text
src/openai_server_main.cpp                         OpenAI HTTP server 入口
src/qwen2_model.cpp / src/qwen2_model.hpp          Qwen2 模型加载和生成
src/model_base.*                                   通用 layer 抽象
src/llama2_layer_cpu.* / src/llama2_layer_npu.*    共享 CPU/NPU layer 实现，Qwen2 仍依赖这些文件
scripts/convert_qwen2_weight.py                    BF16/FP16 Qwen 权重转换
scripts/convert_qwen2_awq_weight.py                AWQ Qwen 权重转换
scripts/start_original_like_openai_server_orangepi.sh
scripts/start_deepseek_r1_qwen2.5_1.5B_openai_server_orangepi.sh
```

注意：部分底层文件名仍包含 `llama2_layer`，这是原项目的共享 layer 命名；当前 server 只构建 `qwen_openai_server`。

## 权重转换

BF16/FP16 Qwen 模型：

```bash
python3 scripts/convert_qwen2_weight.py \
  --input_model_path /root/models/DeepSeek-R1-Distill-Qwen-1.5B \
  --output_dir /root/models/DeepSeek-R1-Distill-Qwen-1.5B_converted
```

AWQ Qwen 模型：

```bash
python3 scripts/convert_qwen2_awq_weight.py \
  --input_model_path /path/to/Qwen2.5-7B-Instruct-AWQ \
  --output_dir /path/to/Qwen2.5-7B-Instruct-AWQ_converted
```

## 编译

```bash
cmake -S . -B build_server -DCMAKE_BUILD_TYPE=Release
cmake --build build_server -j2
```

生成二进制：

```text
build_server/src/qwen_openai_server
```

## 启动

原始脚本参数等价版本，适合和原始 text completion 速度对比：

```bash
bash scripts/start_original_like_openai_server_orangepi.sh
```

长上下文版本：

```bash
bash scripts/start_deepseek_r1_qwen2.5_1.5B_openai_server_orangepi.sh
```

如需修改模型路径、端口、`max_seq_len` 或 `max_gen_token`，直接编辑 `scripts/start_*.sh`。

## 非流式请求

```bash
curl http://127.0.0.1:8081/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "deepseek-r1-distill-qwen-1.5b",
    "messages": [
      {"role": "user", "content": "介绍一下南开大学"}
    ],
    "max_tokens": 128,
    "temperature": 0.6,
    "top_p": 0.9
  }'
```

默认情况下，`/v1/chat/completions` 只取最后一条 `user` 消息作为 raw prompt，这样和原始 text completion 行为一致。

如果需要 Qwen chat template，可以启动时加：

```bash
--use_chat_template=true
```

## 流式请求

```bash
curl -N http://127.0.0.1:8081/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "deepseek-r1-distill-qwen-1.5b",
    "messages": [
      {"role": "user", "content": "介绍一下南开大学"}
    ],
    "max_tokens": 128,
    "temperature": 0.6,
    "top_p": 0.9,
    "stream": true
  }'
```

流式返回使用 SSE：

```text
data: {"object":"chat.completion.chunk", ...}

data: [DONE]
```

流式模式每个 token 都要组 JSON 并写 socket，速度通常会低于非流式。对比原始推理速度时建议使用非流式请求，并查看日志中的 `decode_tps`。

