#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IMAGE:-qwen-openai-server:orangepi}"
NAME="${NAME:-qwen-openai-server}"
MODEL_DIR="${MODEL_DIR:-/root/models/DeepSeek-R1-Distill-Qwen-1.5B_server}"
PORT="${PORT:-8081}"

DOCKER_ARGS=(
  --rm
  -it
  --name "${NAME}"
  --network host
  --privileged
  -e ASCEND_VISIBLE_DEVICES="${ASCEND_VISIBLE_DEVICES:-0}"
  -e ASCEND_RT_VISIBLE_DEVICES="${ASCEND_RT_VISIBLE_DEVICES:-0}"
  -e ASCEND_ALLOW_LINK="${ASCEND_ALLOW_LINK:-True}"
  -e MODEL_DIR=/models/DeepSeek-R1-Distill-Qwen-1.5B_server
  -e PORT="${PORT}"
  -e MAX_SEQ_LEN="${MAX_SEQ_LEN:-8192}"
  -e MAX_GEN_TOKEN="${MAX_GEN_TOKEN:-8192}"
  -v "${MODEL_DIR}:/models/DeepSeek-R1-Distill-Qwen-1.5B_server:ro"
)

add_device() {
  local src="$1"
  local dst="${2:-$1}"
  if [ -e "${src}" ]; then
    DOCKER_ARGS+=(--device="${src}:${dst}")
  fi
}

add_mount() {
  local src="$1"
  local dst="${2:-$1}"
  local mode="${3:-}"
  if [ -e "${src}" ]; then
    if [ -n "${mode}" ]; then
      DOCKER_ARGS+=(-v "${src}:${dst}:${mode}")
    else
      DOCKER_ARGS+=(-v "${src}:${dst}")
    fi
  fi
}

add_device /dev/davinci0
add_device /dev/davinci_manager_docker /dev/davinci_manager
add_device /dev/ascend_manager
add_device /dev/svm0
add_device /dev/devmm_svm
add_device /dev/ts_aisle
add_device /dev/upgrade
add_device /dev/sys
add_device /dev/vdec
add_device /dev/vpc
add_device /dev/pngd
add_device /dev/venc
add_device /dev/dvpp_cmdlist
add_device /dev/log_drv

add_mount /usr/local/Ascend /usr/local/Ascend ro
add_mount /usr/local/Ascend/driver /usr/local/Ascend/driver
add_mount /usr/local/Ascend/driver/lib64 /usr/local/Ascend/driver/lib64
add_mount /usr/local/Ascend/ascend-toolkit /usr/local/Ascend/ascend-toolkit ro
add_mount /var/davinci /var/davinci
add_mount /var/davinci/driver /var/davinci/driver
add_mount /usr/lib64 /usr/lib64 ro
add_mount /usr/lib64/aicpu_kernels /usr/lib64/aicpu_kernels ro
add_mount /etc/sys_version.conf /etc/sys_version.conf ro
add_mount /etc/hdcBasic.cfg /etc/hdcBasic.cfg ro
add_mount /etc/ascend_install.info /etc/ascend_install.info ro
add_mount /etc/slog.conf /etc/slog.conf ro
add_mount /var/slogd /var/slogd
add_mount /var/dmp_daemon /var/dmp_daemon
add_mount /var/log/ascend_seclog /var/log/ascend_seclog
add_mount /usr/local/sbin/npu-smi /usr/local/sbin/npu-smi ro

for lib in \
  libaicpu_processer.so \
  libaicpu_prof.so \
  libaicpu_sharder.so \
  libadump.so \
  libtsd_eventclient.so \
  libaicpu_scheduler.so \
  libdcmi.so \
  libmpi_dvpp_adapter.so \
  libstackcore.so \
  libc_sec.so \
  libdevmmap.so \
  libdrvdsmi.so \
  libslog.so \
  libmmpa.so \
  libascend_hal.so; do
  add_mount "/usr/lib64/${lib}" "/usr/lib64/${lib}" ro
done

exec docker run "${DOCKER_ARGS[@]}" "${IMAGE}"
