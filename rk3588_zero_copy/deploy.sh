#!/bin/bash
# ============================================================================
# 上板验证: 连通性检查 -> 环境自检 -> 传源码/模型 -> 板端编译 -> 运行
#
# 用法:
#   IP=192.168.31.77 ./deploy.sh              # 全流程(板端编译 + 跑)
#   IP=192.168.31.77 SKIP_BUILD=1 ./deploy.sh # 只上传 + 运行(本地已编好)
#   IP=192.168.31.77 RUN_ARGS="--frames 600" ./deploy.sh
# ============================================================================
set -e

ROOT_PWD=$(cd "$(dirname "$0")" && pwd)
: ${IP:=192.168.31.77}
: ${SSH_USER:=seed}
: ${RK_DEMO_ROOT:=${ROOT_PWD}/../yolov8-helmet-rk3588-multithread}
: ${REMOTE_DIR:=/root/rk3588_zero_copy}
: ${REMOTE_MODEL_DIR:=/root/model}

# ---- 模型与标签 (9 输出 DFL 模型, 类别: head / helmet) --------------------
MODEL_NAME=best.rknn
LABELS_NAME=labels_list.txt
MODEL_SRC="${RK_DEMO_ROOT}/model/${MODEL_NAME}"
LABELS_SRC="${RK_DEMO_ROOT}/model/${LABELS_NAME}"
REMOTE_MODEL="${REMOTE_MODEL_DIR}/${MODEL_NAME}"
REMOTE_LABELS="${REMOTE_MODEL_DIR}/${LABELS_NAME}"

# ---- RTSP (项目里的相机) ---------------------------------------------------
: ${RTSP:=rtsp://192.168.31.200:8557/h264}
: ${CONF:=0.25}
: ${IOU:=0.45}
: ${FRAMES:=300}
: ${RUN_ARGS:=--no-show --frames ${FRAMES}}

SSH="ssh -o ConnectTimeout=10 -o ServerAliveInterval=15 -o StrictHostKeyChecking=no ${SSH_USER}@${IP}"
SCP="scp -o ConnectTimeout=10 -o StrictHostKeyChecking=no"

echo "=========================================================="
echo " 目标板子   : ${IP}"
echo " 模型       : ${MODEL_SRC}"
echo " 标签       : ${LABELS_SRC}"
echo " RTSP       : ${RTSP}"
echo " 运行参数   : ${RUN_ARGS}"
echo "=========================================================="

# ---- 0) 本地文件检查 ------------------------------------------------------
for f in "${MODEL_SRC}" "${LABELS_SRC}"; do
  if [ ! -f "$f" ]; then
    echo "[!!] 本地缺少 $f" >&2
    echo "     RK_DEMO_ROOT=${RK_DEMO_ROOT}" >&2
    exit 1
  fi
done
echo "[0] 本地文件 OK"
echo "    模型大小: $(stat -c%s "${MODEL_SRC}") 字节"
echo "    标签内容: $(tr -d '\r' < "${LABELS_SRC}" | tr '\n' ' ')"

# ---- 1) 连通性 (网络不稳, 重试) ------------------------------------------
echo "[1] 检查 SSH 连通性..."
for i in 1 2 3 4 5; do
  if ${SSH} 'echo ok' 2>/dev/null; then
    echo "    连通 (第 ${i} 次)"
    break
  fi
  echo "    第 ${i} 次失败, 5 秒后重试..."
  sleep 5
  if [ "$i" = "5" ]; then
    echo "[!!] SSH 连不上 ${IP}, 先确认: ping 通? 22 端口通? 板子上的 sshd 起来了?" >&2
    exit 1
  fi
done

# ---- 2) 板端环境自检 ------------------------------------------------------
echo "[2] 板端环境自检..."
${SSH} '
  echo "    -- uname";        uname -m; uname -r
  echo "    -- 编译器";        (g++ --version | head -1) 2>&1
  echo "    -- cmake";        (cmake --version | head -1) 2>&1
  echo "    -- dma_heap";     ls /dev/dma_heap/ 2>&1
  echo "    -- RK 库";        ls -l /usr/lib/librknnrt.so /usr/lib/librga.so /usr/lib/librockchip_mpp.so 2>&1
  echo "    -- RK 头文件";    ls /usr/include/rknn_api.h /usr/include/im2d.h /usr/include/rk_mpp.h 2>&1
  echo "    -- FFmpeg 头";     ls /usr/include/libavformat/avformat.h 2>&1
  echo "    -- OpenCV 头";     ls /usr/include/opencv4/opencv2/core.hpp /usr/include/opencv2/core.hpp 2>&1
  echo "    -- ffmpeg rkmpp";  (ffmpeg -decoders 2>/dev/null | grep rkmpp) || echo "      (没装 ffmpeg 命令行)"
  echo "    -- NPU";           cat /sys/kernel/debug/rknpu/version 2>/dev/null || dmesg 2>/dev/null | grep -i rknpu | tail -1
'

# ---- 3) 传源码 ------------------------------------------------------------
echo "[3] 上传源码..."
${SSH} "mkdir -p ${REMOTE_DIR} ${REMOTE_MODEL_DIR}"
tar czf - -C "${ROOT_PWD}" . | ${SSH} "tar xzf - -C ${REMOTE_DIR}"
echo "    完成"

# ---- 4) 传模型和标签 ------------------------------------------------------
echo "[4] 上传模型(${MODEL_NAME}) 和标签(${LABELS_NAME})..."
${SCP} "${MODEL_SRC}"  seed@"${IP}":${REMOTE_MODEL}
${SCP} "${LABELS_SRC}" seed@"${IP}":${REMOTE_LABELS}
# 校验大小一致
LOCAL_SZ=$(stat -c%s "${MODEL_SRC}")
REMOTE_SZ=$(${SSH} "stat -c%s ${REMOTE_MODEL}")
if [ "${LOCAL_SZ}" != "${REMOTE_SZ}" ]; then
  echo "[!!] 模型大小不一致: 本地 ${LOCAL_SZ} vs 板端 ${REMOTE_SZ}" >&2
  exit 1
fi
echo "    模型 ${REMOTE_MODEL} (${REMOTE_SZ} 字节) 校验一致"
echo "    标签内容: $($(${SSH} "cat ${REMOTE_LABELS}" | tr -d '\r' | tr '\n' ' '))"

# ---- 5) 板端编译 ----------------------------------------------------------
if [ -z "${SKIP_BUILD}" ]; then
  echo "[5] 板端编译..."
  ${SSH} "cd ${REMOTE_DIR} && chmod +x build_native.sh 2>/dev/null; mkdir -p build_native; \
          if command -v cmake >/dev/null 2>&1; then \
              mkdir -p build && cd build && \
              cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4 && \
              cp rk_zero_copy ../build_native/ ; \
          else \
              ./build_native.sh ; \
          fi"
  echo "    编译完成"
  ${SSH} "ls -l ${REMOTE_DIR}/build_native/rk_zero_copy 2>/dev/null || \
          ls -l ${REMOTE_DIR}/build/rk_zero_copy 2>/dev/null"
fi

# ---- 6) 运行 --------------------------------------------------------------
echo "[6] 运行 (${RUN_ARGS})..."
${SSH} "
  export LD_LIBRARY_PATH=/usr/lib:/root/lib:\${LD_LIBRARY_PATH}
  BIN=\$(ls ${REMOTE_DIR}/build_native/rk_zero_copy ${REMOTE_DIR}/build/rk_zero_copy 2>/dev/null | head -1)
  echo \"    可执行文件: \$BIN\"
  \$BIN \
      '${RTSP}' \
      '${REMOTE_MODEL}' \
      --labels '${REMOTE_LABELS}' \
      --conf ${CONF} --nms ${IOU} \
      ${RUN_ARGS}
"
echo ""
echo "=========================================================="
echo " 完成。"
echo "=========================================================="
