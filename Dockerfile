ARG BASE_IMAGE=ubuntu:latest
ARG RUNTIME_IMAGE=${BASE_IMAGE}

FROM ${BASE_IMAGE} AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libboost-filesystem-dev \
    libboost-program-options-dev \
    libboost-system-dev \
    libeigen3-dev \
    libfmt-dev \
    libre2-dev \
    libsentencepiece-dev \
    libspdlog-dev \
    nlohmann-json3-dev \
    python3 \
    python3-dev \
    python3-venv \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

ENV VIRTUAL_ENV=/opt/qwen-venv
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"

RUN python3 -m venv "${VIRTUAL_ENV}" && \
    pip install --no-cache-dir --upgrade pip && \
    pip install --no-cache-dir \
    transformers \
    tokenizers \
    sentencepiece \
    safetensors \
    numpy

WORKDIR /src/qwen_server

COPY . .
COPY --from=ascend_arch . /usr/local/Ascend/ascend-toolkit/latest/aarch64-linux
COPY --from=ascend_driver_lib64 . /usr/local/Ascend/driver/lib64

RUN test -f /usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/include/acl/acl.h && \
    test -f /usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/lib64/libascendcl.so && \
    test -f /usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/lib64/libruntime.so && \
    test -f /usr/local/Ascend/driver/lib64/libascend_hal.so && \
    cmake -S . -B build_server -DCMAKE_BUILD_TYPE=Release \
      -DACL_PATH=/usr/local/Ascend/ascend-toolkit/latest/aarch64-linux \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,/usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/lib64 -Wl,-rpath-link,/usr/local/Ascend/driver/lib64" && \
    cmake --build build_server -j2

FROM ${RUNTIME_IMAGE} AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libboost-filesystem-dev \
    libboost-program-options-dev \
    libboost-system-dev \
    libfmt-dev \
    libre2-dev \
    libsentencepiece-dev \
    libspdlog-dev \
    python3 \
    python3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app/qwen_server

COPY --from=builder /src/qwen_server/build_server/src/qwen_openai_server /app/qwen_server/qwen_openai_server
COPY --from=builder /opt/qwen-venv /opt/qwen-venv
COPY prebuild/libnpu_ops.so /app/qwen_server/lib/libnpu_ops.so
COPY docker/entrypoint.sh /app/qwen_server/entrypoint.sh
COPY scripts/start_deepseek_r1_qwen2.5_1.5B_openai_server_orangepi.sh  /app/qwen_server/scripts/start_deepseek_r1_qwen2.5_1.5B_openai_server_orangepi.sh 

RUN chmod +x /app/qwen_server/qwen_openai_server /app/qwen_server/entrypoint.sh  /app/qwen_server/scripts/start_deepseek_r1_qwen2.5_1.5B_openai_server_orangepi.sh 

ENV ASCEND_TOOLKIT_HOME=/usr/local/Ascend/ascend-toolkit/latest
ENV ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
ENV ASCEND_AICPU_PATH=/usr/local/Ascend/ascend-toolkit/latest
ENV ASCEND_OPP_PATH=/usr/local/Ascend/ascend-toolkit/latest/opp
ENV TOOLCHAIN_HOME=/usr/local/Ascend/ascend-toolkit/latest/toolkit
ENV ASCEND_RT_VISIBLE_DEVICES=0
ENV VIRTUAL_ENV=/opt/qwen-venv
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"
ENV LD_LIBRARY_PATH=/app/qwen_server/lib:/opt/qwen-venv/lib:/usr/local/Ascend/ascend-toolkit/latest/tools/aml/lib64:/usr/local/Ascend/ascend-toolkit/latest/tools/aml/lib64/plugin:/usr/local/Ascend/ascend-toolkit/latest/lib64:/usr/local/Ascend/ascend-toolkit/latest/lib64/plugin/opskernel:/usr/local/Ascend/ascend-toolkit/latest/lib64/plugin/nnengine:/usr/local/Ascend/ascend-toolkit/latest/opp/built-in/op_impl/ai_core/tbe/op_tiling/lib/linux/aarch64:/usr/local/Ascend/driver/lib64:/var/davinci/driver/lib64:/var/davinci/driver/lib64/common:/var/davinci/driver/lib64/driver:/usr/lib64:/lib64:${LD_LIBRARY_PATH}
ENV PYTHONPATH=/app/qwen_server:/opt/qwen-venv/lib/python3.12/site-packages:/opt/qwen-venv/lib/python3.10/site-packages:/usr/local/Ascend/ascend-toolkit/latest/python/site-packages:/usr/local/Ascend/ascend-toolkit/latest/opp/built-in/op_impl/ai_core/tbe:${PYTHONPATH}

EXPOSE 8081

#ENTRYPOINT ["/app/qwen_server/entrypoint.sh"]
