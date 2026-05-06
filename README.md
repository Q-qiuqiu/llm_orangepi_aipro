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

## Docker 镜像

Docker 镜像使用 multistage 构建，在 builder 阶段安装编译依赖并编译可执行文件，runtime 阶段只保留运行时文件。

构建时需要启用 BuildKit，并把宿主机 Ascend 的 `aarch64-linux` 目录作为额外 build context 传给 builder 阶段。

宿主机需要存在：

```text
/usr/local/Ascend/ascend-toolkit/8.0.0/aarch64-linux
/usr/local/Ascend/driver/lib64
```

构建命令：

```bash
DOCKER_BUILDKIT=1 docker build \
  --build-context ascend_arch=/usr/local/Ascend/ascend-toolkit/8.0.0/aarch64-linux \
  --build-context ascend_driver_lib64=/usr/local/Ascend/driver/lib64 \
  -t qwen-openai-server:orangepi .
```

`BASE_IMAGE` 默认是 `ubuntu:latest`，builder 和 runtime 默认都使用这个基础镜像。如果你想换成本地其他 Ubuntu 镜像，可以传入：

```bash
DOCKER_BUILDKIT=1 docker build \
  --build-context ascend_arch=/usr/local/Ascend/ascend-toolkit/8.0.0/aarch64-linux \
  --build-context ascend_driver_lib64=/usr/local/Ascend/driver/lib64 \
  --build-arg BASE_IMAGE=你的本地Ubuntu镜像 \
  -t qwen-openai-server:orangepi .
```

注意：Ascend toolkit 只在编译阶段临时挂载使用，不会被复制进最终镜像。runtime 容器启动时仍然需要挂载宿主机 Ascend runtime/driver 和 NPU 设备节点。

运行容器时需要挂载宿主机 Ascend runtime/driver、NPU 设备节点和模型部署目录。推荐直接使用脚本，脚本会参考 Orange Pi 上可用的 Ascend Docker 挂载方式，并自动跳过不存在的设备或文件：

```bash
docker run -it \
    --name qwen-openai-server \
    --network host \
    --privileged \
    -e ASCEND_VISIBLE_DEVICES=0 \
    -e ASCEND_ALLOW_LINK=True \
    -e MODEL_DIR=/root/models/DeepSeek-R1-Distill-Qwen-1.5B_server \
    -e PORT=8081 \
    --device=/dev/svm0 \
    --device=/dev/ts_aisle \
    --device=/dev/upgrade \
    --device=/dev/sys \
    --device=/dev/log_drv \
    -v /root/models/DeepSeek-R1-Distill-Qwen-1.5B_server:/root/models/DeepSeek-R1-Distill-Qwen-1.5B_server:ro \
    -v /usr/local/Ascend:/usr/local/Ascend:ro \
    -v /var/davinci:/var/davinci \
    -v /usr/lib64:/usr/lib64:ro \
    -v /etc/sys_version.conf:/etc/sys_version.conf:ro \
    -v /etc/hdcBasic.cfg:/etc/hdcBasic.cfg:ro \
    -v /var/slogd:/var/slogd \
    -v /var/dmp_daemon:/var/dmp_daemon \
    -v /usr/local/Ascend/driver:/usr/local/Ascend/driver \
    -v /var/davinci/driver:/var/davinci/driver \
    qwen-openai-server:orangepi

```

默认模型部署目录是 `/root/models/DeepSeek-R1-Distill-Qwen-1.5B_server`，默认端口是 `8081`。

容器启动参数通过环境变量配置：

|变量|默认值|说明|
|---|---|---|
|`MODEL_DIR`|`/models/DeepSeek-R1-Distill-Qwen-1.5B_server`|部署目录，包含 `config/`、`tokenizer/`、`converted/`|
|`CONFIG_PATH`|`${MODEL_DIR}/config/config.json`|模型 config 路径|
|`TOKENIZER_DIR`|`${MODEL_DIR}/tokenizer`|tokenizer 目录|
|`WEIGHT_DIR`|`${MODEL_DIR}/converted`|转换后的 `.bin` 权重目录|
|`HOST`|`0.0.0.0`|监听地址|
|`PORT`|`8081`|监听端口|
|`SERVED_MODEL_NAME`|`deepseek-r1-distill-qwen-1.5b`|OpenAI 响应里的模型名|
|`MAX_SEQ_LEN`|`8192`|最大上下文长度|
|`MAX_GEN_TOKEN`|`8192`|默认生成 token 数|
|`TEMPERATURE`|`0.6`|默认采样温度|
|`TOP_P`|`0.9`|默认 top-p|
|`USE_CHAT_TEMPLATE`|`false`|是否使用 Qwen chat template|
|`QUANT_METHOD`|空|AWQ 模型可设为 `awq_4bit`|

镜像内只包含 `qwen_openai_server`、`libnpu_ops.so`、Python venv 和 C++ 运行库。运行容器时需要挂载宿主机 Ascend runtime/driver 和 NPU 设备节点。

推荐的模型部署目录结构：

```text
DeepSeek-R1-Distill-Qwen-1.5B_server/
├── config/
│   ├── config.json
│   └── generation_config.json
├── tokenizer/
│   ├── tokenizer.json
│   └── tokenizer_config.json
└── converted/
    ├── model.embed_tokens.weight.bin
    ├── model.layers.0...
    └── ...
```

这个目录不需要包含原始 `model.safetensors`。

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
