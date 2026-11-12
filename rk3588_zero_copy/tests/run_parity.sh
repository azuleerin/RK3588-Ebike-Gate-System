#!/bin/bash
# run_parity.sh —— C++ 板端逻辑 vs Python 基线 对拍
#
# 用法:
#   ./run_parity.sh              # 用 docker 编译 C++ 再对比
#   SKIP_DOCKER=1 ./run_parity.sh  # 已经有编译好的 tests/test_speed 时
#
# 需要: docker(带 g++) 或本机 g++；python3 + numpy + opencv(跑 py_reference.py)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/.." && pwd)
SD="${SD:-$(cd "${ROOT}/.." && pwd)/speed_demo.py}"

echo "== 1) 生成 Python 基线 (${SD}) =="
python3 "${HERE}/py_reference.py" "${SD}" > "${HERE}/out_py.txt"
echo "   $(wc -l < "${HERE}/out_py.txt") 行"

echo "== 2) 编译并运行 C++ =="
if [ -z "$SKIP_DOCKER" ] && command -v docker >/dev/null 2>&1; then
  WD=$(cd "${ROOT}" && pwd -W)
  docker run --rm -v "${WD}:/work" --entrypoint bash ubuntu/squid:latest -c "
    apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq g++ >/dev/null 2>&1
    cd /work && g++ -O2 -std=c++17 -Wall -o /tmp/ts \
        tests/test_speed.cpp src/ipm.cc src/speed_track.cc src/yolo_post.cc -Iinclude \
      && /tmp/ts > tests/out_cpp.txt
  "
else
  g++ -O2 -std=c++17 -Wall -o "${HERE}/test_speed" \
      "${HERE}/test_speed.cpp" "${ROOT}/src/ipm.cc" "${ROOT}/src/speed_track.cc" \
      "${ROOT}/src/yolo_post.cc" -I"${ROOT}/include"
  "${HERE}/test_speed" > "${HERE}/out_cpp.txt"
fi
echo "   $(wc -l < "${HERE}/out_cpp.txt") 行"

echo "== 3) 对比 =="
python3 "${HERE}/compare.py" "${HERE}/out_cpp.txt" "${HERE}/out_py.txt"
