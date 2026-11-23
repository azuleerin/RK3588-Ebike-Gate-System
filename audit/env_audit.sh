#!/usr/bin/env bash
# env_audit.sh —— 板端运行环境审计 (项3)
#
#   3.1 运行库版本对齐   板端 librknnrt.so 版本 vs 模型量化所用 RKNN Toolkit 版本
#   3.2 NPU 调频策略     governor / 当前主频 / 最高主频, 非 performance 则给出定频命令
#   3.3 内存零拷贝确认   主工程推理代码的输入内存分配与绑定方式, 排查隐式 memcpy
#
# 只读审计: 不需要 root, 不改任何配置。
# 用法:
#   ./env_audit.sh [--project /home/seeed/rk_deploy/rk3588_zero_copy]
#                  [--model   /home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn]
set -u

PROJECT="/home/seeed/rk_deploy/rk3588_zero_copy"
MODEL="/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn"
while [ $# -gt 0 ]; do
    case "$1" in
        --project) PROJECT="$2"; shift 2 ;;
        --model)   MODEL="$2"; shift 2 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

RC=0          # 0 = 审计通过, 1 = 发现硬问题
NOTE=""       # 硬问题汇总
RISK=""       # 风险项汇总 (不一定是当前的实际损耗)

hr()  { printf '=%.0s' {1..78}; echo; }
sec() { echo; hr; echo "$1"; hr; }

# ---------------------------------------------------------------- 3.1 版本对齐
sec "3.1  运行库版本对齐 (librknnrt.so  vs  模型量化 Toolkit)"

RUNTIME_SO=""
for c in /usr/lib/librknnrt.so "$PROJECT/third_party/lib/librknnrt.so"; do
    [ -e "$c" ] && { RUNTIME_SO="$c"; break; }
done
if [ -z "$RUNTIME_SO" ]; then
    echo "  【问题】找不到 librknnrt.so"
    RC=1; NOTE="$NOTE; 运行库缺失"
else
    RT_VER=$(strings "$RUNTIME_SO" 2>/dev/null | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' | head -1)
    RT_BUILD=$(strings "$RUNTIME_SO" 2>/dev/null | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+ \([^)]*\)' | head -1)
    echo "  板端运行库      : $RUNTIME_SO"
    echo "  运行库版本      : ${RT_VER:-未知}   (${RT_BUILD:-})"
    echo "  运行库大小/时间 : $(stat -c '%s bytes  %y' "$RUNTIME_SO" 2>/dev/null)"
    if [ -n "$PROJECT/third_party/lib/librknnrt.so" ]; then
        A=$(md5sum "$RUNTIME_SO" | cut -d' ' -f1)
        B=$(md5sum "$PROJECT/third_party/lib/librknnrt.so" | cut -d' ' -f1)
        if [ "$A" = "$B" ]; then
            echo "  与主工程自带库   : 一致 (md5 相同)  ✓"
        else
            echo "  与主工程自带库   : 不一致 (md5 不同)  ← 注意"
            echo "                    板端 /usr/lib: $A"
            echo "                    工程 third_party: $B"
            RC=1; NOTE="$NOTE; 运行库版本不一致"
        fi
    fi
fi

# 模型编译版本
MODEL_VER=""; MODEL_TARGET=""
if [ -e "$MODEL" ]; then
    MODEL_VER=$(strings "$MODEL" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\(compiler version:[^)]*\)\)' | head -1)
    MODEL_TARGET=$(strings "$MODEL" 2>/dev/null | grep -oE 'rk3588|rk3568|rk3576|rk2332' | head -1)
    echo "  模型文件        : $MODEL"
    echo "  模型编译器版本  : ${MODEL_VER:-未知}"
    echo "  模型目标平台    : ${MODEL_TARGET:-未知}"
    MV=$(echo "$MODEL_VER" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+')
else
    echo "  【问题】模型文件不存在: $MODEL"
    RC=1; NOTE="$NOTE; 模型缺失"
fi

# 大版本比对
if [ -n "${RT_VER:-}" ] && [ -n "${MV:-}" ]; then
    RT_MAJ=$(echo "$RT_VER" | cut -d. -f1); RT_MIN=$(echo "$RT_VER" | cut -d. -f2)
    MV_MAJ=$(echo "$MV" | cut -d. -f1); MV_MIN=$(echo "$MV" | cut -d. -f2)
    echo
    if [ "$RT_VER" = "$MV" ]; then
        echo "  >> 运行库($RT_VER) 与模型编译器($MV) 【大版本严格匹配】 ✓ 无版本倒挂"
    elif [ "$RT_MAJ" != "$MV_MAJ" ]; then
        echo "  >> 【问题】大版本不匹配: 运行库 $RT_VER vs 模型 $MV —— 存在版本倒挂风险"
        RC=1; NOTE="$NOTE; 运行库/模型大版本不匹配"
    elif [ "$RT_MIN" != "$MV_MIN" ]; then
        echo "  >> 【注意】次版本不同: 运行库 $RT_VER vs 模型 $MV (主版本一致, 跨度 1 个次版本)"
        echo "     RKNN 2.x 同主版本内向后兼容, 风险可控, 但建议用与模型同版本的 Toolkit 重编"
    else
        echo "  >> 运行库($RT_VER) 与模型($MV) 兼容 ✓"
    fi
fi

# ---------------------------------------------------------------- 3.2 调频策略
sec "3.2  NPU / CPU / DDR 调频策略"

NPU_DIR=$(ls -d /sys/class/devfreq/*npu* 2>/dev/null | head -1)
if [ -n "$NPU_DIR" ]; then
    GOV=$(cat "$NPU_DIR/governor" 2>/dev/null)
    CUR=$(cat "$NPU_DIR/cur_freq" 2>/dev/null)
    MAX=$(cat "$NPU_DIR/max_freq" 2>/dev/null)
    MIN=$(cat "$NPU_DIR/min_freq" 2>/dev/null)
    LOAD=$(cat "$NPU_DIR/load" 2>/dev/null | head -1)
    echo "  NPU devfreq     : $NPU_DIR"
    echo "  governor        : $GOV"
    echo "  当前主频        : $CUR Hz"
    echo "  频率区间        : $MIN .. $MAX Hz"
    echo "  负载            : $LOAD"
    echo "  可用 governor   : $(cat "$NPU_DIR/available_governors" 2>/dev/null)"
    echo
    if [ "$GOV" = "performance" ]; then
        echo "  >> 【无问题】NPU 已定频 performance。"
    else
        echo "  >> 【有问题】NPU governor = '$GOV' (非 performance)。"
        echo "     风险: 帧间空档会让 NPU 降频, 下一帧等升频爬坡 —— 表现为 npu_ms 抖动大 / P99 难看。"
        echo "     定频命令 (需要 root):"
        echo "         echo performance > $NPU_DIR/governor"
        echo "         echo $MAX > $NPU_DIR/min_freq      # 兜底: min=max 直接钉死"
        echo "     项目现成脚本: sudo $PROJECT/scripts/lock_max_freq.sh --lock   (还原用 --restore)"
        if [ "$CUR" = "$MAX" ]; then
            RISK="$RISK; NPU governor=$GOV (当前 cur_freq 已等于 max_freq, 满载下无实际损耗, 空闲/低负载段有降频爬坡风险)"
            echo "     ※ 但当前实测 cur_freq == max_freq ($CUR): 满负载下已在最高频运行,"
            echo "       故【当前】不构成实际损耗; 风险在于负载有空档时才会显现。"
        else
            RC=1; NOTE="$NOTE; NPU governor 非 performance 且 cur_freq($CUR) < max_freq($MAX)"
        fi
    fi
else
    echo "  【注意】未找到 NPU devfreq 节点"
fi

echo
echo "  -- CPU --"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    echo "  $(basename "$p") cpus=[$(cat "$p/affected_cpus" 2>/dev/null)] gov=$(cat "$p/scaling_governor" 2>/dev/null) cur=$(cat "$p/scaling_cur_freq" 2>/dev/null) max=$(cat "$p/scaling_max_freq" 2>/dev/null)"
done
echo "  -- DDR (dmc) --"
for d in /sys/class/devfreq/dmc; do
    [ -d "$d" ] || continue
    echo "  $(cat "$d/name" 2>/dev/null) gov=$(cat "$d/governor" 2>/dev/null) cur=$(cat "$d/cur_freq" 2>/dev/null) max=$(cat "$d/max_freq" 2>/dev/null)"
done

# ---------------------------------------------------------------- 3.3 零拷贝
sec "3.3  内存零拷贝链路确认 (主工程推理代码静态审计)"

if [ ! -d "$PROJECT/src" ]; then
    echo "  【问题】工程源码目录不存在: $PROJECT/src"
    RC=1
else
    echo "  -- 3.3.1 NPU 输入内存的分配与绑定方式 --"
    echo "  (rknn_create_mem* / rknn_set_io_mem = 零拷贝; rknn_inputs_set = 隐式 memcpy)"
    echo
    for f in "$PROJECT"/src/*.cc "$PROJECT"/src/rk_base/*.cc; do
        [ -e "$f" ] || continue
        hits=$(grep -n -E "rknn_create_mem|rknn_create_mem_from_fd|rknn_set_io_mem|rknn_inputs_set" "$f" 2>/dev/null)
        [ -z "$hits" ] && continue
        echo "  ▶ $(realpath --relative-to="$PROJECT" "$f")"
        echo "$hits" | sed 's/^/      /'
        echo
    done

    echo "  -- 3.3.2 汇总判定 --"
    ZC=$(grep -rn "rknn_set_io_mem" "$PROJECT/src" 2>/dev/null | wc -l)
    MC=$(grep -rn "rknn_inputs_set" "$PROJECT/src" 2>/dev/null | wc -l)
    FD=$(grep -rn "rknn_create_mem_from_fd" "$PROJECT/src" 2>/dev/null | wc -l)
    MM=$(grep -rn "rknn_create_mem(" "$PROJECT/src" 2>/dev/null | wc -l)
    echo "  rknn_create_mem_from_fd 调用点 : $FD"
    echo "  rknn_create_mem      调用点    : $MM"
    echo "  rknn_set_io_mem      调用点    : $ZC"
    echo "  rknn_inputs_set      调用点    : $MC   (← 隐式 memcpy 嫌疑)"

    # 检测路径(不含 OCR)是否有 inputs_set
    DET_IN=$(grep -rn "rknn_inputs_set" "$PROJECT/src/rk_base/npu_pool.cc" "$PROJECT/src/rk_base/npu_pipe.cc" "$PROJECT/src/gate_main.cc" 2>/dev/null | wc -l)
    OCR_IN=$(grep -rn "rknn_inputs_set" "$PROJECT/src/license_plate_rec.cc" 2>/dev/null | wc -l)
    MAIN_IN=$(grep -rn "rknn_inputs_set" "$PROJECT/src/main.cc" 2>/dev/null | wc -l)
    echo
    echo "  按路径细分:"
    echo "    检测-多进程池 npu_pool.cc : inputs_set=$DET_IN  (0 = 纯零拷贝)"
    echo "    检测-单进程   main.cc     : inputs_set=$MAIN_IN  (兜底分支, 非主路径)"
    echo "    识别-OCR      license_plate_rec.cc : inputs_set=$OCR_IN  (ROI 上传, 小数据量)"
    echo
    if [ "$DET_IN" = "0" ] && [ "$ZC" -gt 0 ]; then
        echo "  >> 检测主路径: 【零拷贝链路完整】✓"
        echo "     输入 dma-buf -> rknn_create_mem_from_fd -> rknn_set_io_mem, 全程无 CPU memcpy。"
    else
        echo "  >> 检测主路径: 【有问题】检测路径存在 rknn_inputs_set"
        RC=1; NOTE="$NOTE; 检测路径存在隐式 memcpy"
    fi
    if [ "$OCR_IN" != "0" ]; then
        echo "  >> 识别(OCR)路径: 走 rknn_inputs_set —— 每次把 ROI 上传到 NPU 输入缓冲"
        echo "     属【小数据量 memcpy】(ROI 约 160×48×3), 不是主路径瓶颈, 但可优化为 create_mem 零拷贝。"
    fi

    echo
    echo "  -- 3.3.3 输入缓冲对齐 / 内存属性 --"
    grep -n -E "dma_buf_alloc|DMA_HEAP_UNCACHED|memalign|aligned_alloc|align" \
        "$PROJECT/src/dmabuf.cc" "$PROJECT/src/rk_base/npu_pool.cc" 2>/dev/null | head -20 \
        | sed 's/^/      /'
    echo
    grep -rn "对齐\|64B\|64 字节\|uncached\|非缓存" "$PROJECT/src/rk_base/npu_pool.cc" 2>/dev/null | head -8 | sed 's/^/      /'

    echo
    echo "  -- 3.3.4 RGA 输出缓冲与 NPU 输入缓冲是否同一块物理内存 --"
    if grep -rn "slot_fd(slot)" "$PROJECT/src/gate_main.cc" 2>/dev/null | head -3 | sed 's/^/      /'; then :; fi
    grep -rn "零拷贝审计" "$PROJECT/src" 2>/dev/null | head -5 | sed 's/^/      /'
fi

# ---------------------------------------------------------------- 附: NPU 占用
sec "附:  当前 NPU 占用与竞争者 (影响基线测量)"

if [ -r /sys/class/devfreq/fdab0000.npu/load ]; then
    echo "  NPU load: $(cat /sys/class/devfreq/fdab0000.npu/load | head -1)"
fi
echo "  正在占用 NPU 的进程:"
found=0
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    [ -r "$p/cmdline" ] || continue
    cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$cmd" in
        *rknn*|*rk_gate*|*rk_zero_copy*|*npu_bench*|*npu_baseline_bench*)
            cpu=$(ps -p "$pid" -o pcpu= 2>/dev/null | tr -d ' ')
            echo "    PID $pid  CPU=${cpu}%  ${cmd:0:110}"
            found=1 ;;
    esac
done
[ "$found" = "0" ] && echo "    (无)"
echo
echo "  注: 基线测量要求 NPU 无竞争。测量前应停止占用进程, 测完恢复。"

# ---------------------------------------------------------------- 结论
sec "审计结论"
if [ -n "$RISK" ]; then
    echo "  【风险项】(不是当前的实际性能损耗, 但需要固化配置时处理):"
    echo "$RISK" | tr ';' '\n' | sed '/^$/d;s/^/    · /'
fi
if [ "$RC" = "0" ]; then
    echo
    echo "  【无问题】运行库版本对齐 / 零拷贝链路 未发现硬伤"
else
    echo
    echo "  【有问题】发现以下硬问题:"
    echo "$NOTE" | tr ';' '\n' | sed '/^$/d;s/^/    · /'
fi

exit $RC
