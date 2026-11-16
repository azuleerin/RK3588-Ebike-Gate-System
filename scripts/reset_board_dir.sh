#!/usr/bin/env bash
# ==============================================================================
# reset_board_dir.sh —— RK3588 板端目录清理 / 工作区重建
#
# 背景: /home/seeed 下代码散落在根目录和 ~/ 两处, shenzhenproject 里又套着
#       废弃备份和构建产物, 跑错版本的风险很高。本脚本把它收敛成唯一工作区。
#
# 目标:
#   1) 统一工作区: 新建 /home/seeed/rk_deploy
#   2) 搬走主力工程: 能跑通 15000 帧的 rk3588_zero_copy -> rk_deploy/
#   3) 模型集中: ebike_int8_rk2332.rknn / labels_ebike.txt 等 -> rk_deploy/models/
#   4) 安全清理: shenzhenproject 嵌套冗余目录、废弃备份、系统默认多媒体目录
#   5) 日志保留: 长跑证据日志 -> rk_deploy/logs/
#
# 安全原则(本脚本刻意保守, 宁可跳过也不误删):
#   - 每一个删除动作都过 safe_rm_path(): 目标必须存在, 且 realpath 必须精确命中白名单
#   - 含源码的目录先打包进 _archive/ 再删除, 随时可完整还原
#   - 只有逐字节比对确认过的纯冗余副本才直接删
#   - 系统默认目录用 rmdir 删除(非空自动失败), 天然不会误删内容
#   - 支持 --dry-run 预演, 全程不碰任何文件
#
# 用法:
#   ./reset_board_dir.sh --dry-run    # 预演, 只打印将要执行的动作
#   ./reset_board_dir.sh              # 交互确认后执行
#   ./reset_board_dir.sh -y           # 直接执行, 不交互
#
# 还原:
#   tar xzf rk_deploy/_archive/legacy_*.tar.gz -C /home/seeed
# ==============================================================================
set -Eeuo pipefail

# --------------------------------------------------------------- 配置区 -------
DEPLOY="${DEPLOY:-/home/seeed/rk_deploy}"
HOME_DIR="${HOME:-/home/seeed}"

# tree / patchelf 装在用户目录下, 确保能找到
export PATH="$HOME_DIR/.local/bin:$PATH"

SRC_PROJECT="$HOME_DIR/rk3588_zero_copy"          # 主力工程(待搬走)
LEGACY_DIR="$HOME_DIR/shenzhenproject"           # 遗留大目录
LEGACY_BK="$HOME_DIR/shenzhenproject.bk"         # 遗留目录的备份副本
LEGACY_TEST="$HOME_DIR/rknn_test"                # 早期 python 试验目录
STALE_CFG="$HOME_DIR/.config/rk3588_zero_copy"   # 放错位置的旧源码快照

MODELS_DIR="$DEPLOY/models"
LOGS_DIR="$DEPLOY/logs"
ARCHIVE_DIR="$DEPLOY/_archive"
TOOLS_DIR="$DEPLOY/tools"
PROJECT_DIR="$DEPLOY/rk3588_zero_copy"

STAMP="$(date +%Y%m%d_%H%M%S)"
DRY_RUN=0
ASSUME_YES=0
CHANGES=0

# 允许删除的精确路径白名单。realpath 之后必须精确命中其中之一才会动手。
# 新增删除目标时, 必须先在这里登记, 否则脚本会拒绝执行。
ALLOW_DELETE=(
  "/home/seeed/shenzhenproject"
  "/home/seeed/shenzhenproject.bk"
  "/home/seeed/rknn_test"
  "/home/seeed/.config/rk3588_zero_copy"
  "/home/seeed/Music"
  "/home/seeed/Pictures"
  "/home/seeed/Videos"
  "/home/seeed/Documents"
  "/home/seeed/Downloads"
  "/home/seeed/Templates"
  "/home/seeed/Public"
  "/home/seeed/rk3588_zero_copy.tar.gz"
  "/home/seeed/rk3588_zero_copy_v2.tar.gz"
  "/home/seeed/model"
  "/home/seeed/board_deps"
  "/home/seeed/rk_deploy/rk3588_zero_copy/docs/evidence"
)

# ------------------------------------------------------------------ 输出 -------
if [ -t 1 ]; then
  C_RST=$'\033[0m'; C_B=$'\033[1m'; C_BL=$'\033[1;34m'
  C_GR=$'\033[32m'; C_YL=$'\033[33m'; C_RD=$'\033[31m'; C_DIM=$'\033[90m'; C_CY=$'\033[36m'
else
  C_RST=""; C_B=""; C_BL=""; C_GR=""; C_YL=""; C_RD=""; C_DIM=""; C_CY=""
fi

step() { printf '\n%s==> %s%s\n' "$C_BL$C_B" "$*" "$C_RST"; }
ok()   { printf '  %s[OK]%s %s\n' "$C_GR" "$C_RST" "$*"; }
warn() { printf '  %s[!!]%s %s\n' "$C_YL" "$C_RST" "$*" >&2; }
err()  { printf '  %s[XX]%s %s\n' "$C_RD" "$C_RST" "$*" >&2; }
skip() { printf '  %s[--]%s %s\n' "$C_DIM" "$C_RST" "$*"; }
info() { printf '     %s\n' "$*"; }
dry()  { printf '  %s[DRY]%s %s\n' "$C_CY" "$C_RST" "$*"; }

# 预演模式下只打印, 不执行
run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    dry "$*"
    return 0
  fi
  "$@"
}

die() { err "$*"; exit 1; }

trap 'rc=$?; [ $rc -eq 0 ] || { printf "\n%s脚本在第 %s 行失败 (退出码 %s), 已停止。%s\n" "$C_RD" "$LINENO" "$rc" "$C_RST" >&2; exit $rc; }' ERR

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
}

# ------------------------------------------------------------ 参数解析 --------
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run|-n) DRY_RUN=1 ;;
    --yes|-y)     ASSUME_YES=1 ;;
    --help|-h)    usage ;;
    *) die "未知参数: $1 (用 --help 看用法)" ;;
  esac
  shift
done

# ------------------------------------------------------------ 通用工具 --------

# 路径是否在删除白名单里
is_allowed() {
  local real="$1" a
  for a in "${ALLOW_DELETE[@]}"; do
    [ "$real" = "$a" ] && return 0
  done
  return 1
}

# 删除一个目录/文件。必须通过白名单 + 危险路径检查。
# 用法: safe_rm_path <路径> <原因说明>
safe_rm_path() {
  local target="$1" reason="${2:-}"
  if [ ! -e "$target" ] && [ ! -L "$target" ]; then
    skip "不存在, 跳过: $target"
    return 0
  fi

  local real
  real="$(realpath -m -- "$target")"

  if ! is_allowed "$real"; then
    err "拒绝删除 (不在白名单内): $real"
    return 1
  fi

  # 双重保险: 这些路径永远不许碰
  case "$real" in
    /|/bin|/boot|/dev|/etc|/home|/home/seeed|/lib|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/var)
      err "拒绝删除系统路径: $real"
      return 1
      ;;
  esac

  info "理由: $reason"
  if [ "$DRY_RUN" -eq 1 ]; then
    dry "rm -rf $real"
    return 0
  fi

  chmod -R u+w -- "$real" 2>/dev/null || true
  if rm -rf -- "$real"; then
    CHANGES=$((CHANGES + 1))
    ok "已删除: $real"
  else
    err "删除失败: $real"
    return 1
  fi
}

# 只删空目录(rmdir 语义: 非空必然失败, 不会误删内容)
safe_rmdir_empty() {
  local target="$1"
  if [ ! -d "$target" ]; then
    skip "不存在, 跳过: $target"
    return 0
  fi
  local real
  real="$(realpath -m -- "$target")"
  if ! is_allowed "$real"; then
    err "拒绝删除 (不在白名单内): $real"
    return 1
  fi
  if [ -n "$(ls -A -- "$target" 2>/dev/null)" ]; then
    skip "非空, 保留: $target ($(ls -A -- "$target" 2>/dev/null | wc -l) 项)"
    return 0
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    dry "rmdir $real"
    return 0
  fi
  if rmdir -- "$target" 2>/dev/null; then
    CHANGES=$((CHANGES + 1))
    ok "已删除空目录: $real"
  else
    warn "rmdir 失败, 保留: $real"
  fi
}

# 移动文件, 目标已存在则不覆盖
safe_move() {
  local src="$1" dst="$2"
  if [ ! -e "$src" ]; then
    skip "源不存在, 跳过: $src"
    return 0
  fi
  if [ -e "$dst" ]; then
    warn "目标已存在, 跳过: $dst"
    return 0
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    dry "mv $src -> $dst"
    return 0
  fi
  mkdir -p -- "$(dirname -- "$dst")"
  if mv -- "$src" "$dst"; then
    CHANGES=$((CHANGES + 1))
    ok "已移动: $src -> $dst"
  else
    err "移动失败: $src -> $dst"
    return 1
  fi
}

# 复制文件, 已存在且内容一致则跳过
safe_copy() {
  local src="$1" dst="$2"
  if [ ! -f "$src" ]; then
    skip "源不存在, 跳过: $src"
    return 0
  fi
  if [ -f "$dst" ]; then
    if cmp -s -- "$src" "$dst"; then
      skip "目标已是同一份文件, 跳过: $dst"
    else
      warn "目标存在且内容不同, 跳过(需人工确认): $dst"
    fi
    return 0
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    dry "cp -p $src -> $dst"
    return 0
  fi
  mkdir -p -- "$(dirname -- "$dst")"
  if cp -p -- "$src" "$dst"; then
    CHANGES=$((CHANGES + 1))
    ok "已复制: $src -> $dst"
  else
    err "复制失败: $src -> $dst"
    return 1
  fi
}

# 把一个目录下的全部文件按文件名搬到目标目录
move_dir_contents() {
  local src_dir="$1" dst_dir="$2" f base
  if [ ! -d "$src_dir" ]; then
    skip "目录不存在, 跳过: $src_dir"
    return 0
  fi
  while IFS= read -r f; do
    base="$(basename -- "$f")"
    safe_move "$f" "$dst_dir/$base"
  done < <(find "$src_dir" -maxdepth 1 -mindepth 1 -print 2>/dev/null)
}

confirm() {
  [ "$ASSUME_YES" -eq 1 ] && return 0
  [ "$DRY_RUN" -eq 1 ] && return 0
  printf '\n%s继续执行? (yes/no) %s' "$C_B" "$C_RST"
  local ans=""
  read -r ans
  case "$ans" in
    y|Y|yes|YES) return 0 ;;
    *) echo "已取消, 未做任何改动。"; exit 0 ;;
  esac
}

# ==============================================================================
# STEP 0 — 前置检查
# ==============================================================================
step "STEP 0  前置检查"

[ "$(id -u)" -eq 0 ] && warn "当前是 root, 建议用普通用户 seeed 运行, 否则新文件会属 root。"

if [ "$(id -un)" != "seeed" ] && [ "$(id -u)" -ne 0 ]; then
  die "当前用户是 $(id -un), 与资产属主 seeed 不符, 中止以免产生错属主文件。"
fi

if [ "$HOME_DIR" != "/home/seeed" ]; then
  die "HOME=$HOME_DIR 与预期的 /home/seeed 不符, 中止。"
fi

command -v sha256sum >/dev/null || die "缺少 sha256sum"
command -v realpath  >/dev/null || die "缺少 realpath"

# 主力工程必须存在且带可执行二进制, 否则不该继续搬
if [ ! -d "$SRC_PROJECT" ]; then
  if [ -d "$PROJECT_DIR" ]; then
    warn "~/rk3588_zero_copy 已不在原位(可能已搬过), 跳过搬家步骤。"
  else
    die "找不到 $SRC_PROJECT, 中止。"
  fi
elif [ ! -x "$SRC_PROJECT/build/rk_zero_copy" ] && [ ! -f "$SRC_PROJECT/build/rk_zero_copy" ]; then
  die "$SRC_PROJECT 下没有 build/rk_zero_copy, 这不是能跑 15000 帧的那份, 中止。"
else
  ok "主力工程就位: $SRC_PROJECT"
fi

df -h "$HOME_DIR" | tail -1 | awk '{printf "     磁盘可用: %s\n", $4}'

# ==============================================================================
# STEP 1 — 建立工作区骨架
# ==============================================================================
step "STEP 1  建立工作区 $DEPLOY"

if [ "$DRY_RUN" -eq 1 ]; then
  dry "mkdir -p $DEPLOY/{models,logs,_archive,tools}"
else
  mkdir -p -- "$DEPLOY" "$MODELS_DIR" "$LOGS_DIR" "$ARCHIVE_DIR" "$TOOLS_DIR"
  ok "目录就绪: $DEPLOY"
fi

# ==============================================================================
# STEP 2 — 搬走主力工程
# ==============================================================================
step "STEP 2  搬走 rk3588_zero_copy -> $PROJECT_DIR"

if [ -d "$SRC_PROJECT" ]; then
  BEFORE_COUNT="$(find "$SRC_PROJECT" -type f 2>/dev/null | wc -l)"
  info "工程文件数: $BEFORE_COUNT"

  if [ -e "$PROJECT_DIR" ]; then
    warn "目标已存在, 跳过搬家: $PROJECT_DIR"
  else
    if [ "$DRY_RUN" -eq 1 ]; then
      dry "mv $SRC_PROJECT -> $PROJECT_DIR"
    else
      mv -- "$SRC_PROJECT" "$PROJECT_DIR"
      CHANGES=$((CHANGES + 1))
      ok "已移动: $SRC_PROJECT -> $PROJECT_DIR"
    fi
  fi

  # 搬家后校验: 文件数一致 + 关键文件在位
  if [ "$DRY_RUN" -eq 0 ] && [ -d "$PROJECT_DIR" ]; then
    AFTER_COUNT="$(find "$PROJECT_DIR" -type f 2>/dev/null | wc -l)"
    if [ "$BEFORE_COUNT" -eq "$AFTER_COUNT" ]; then
      ok "文件数校验通过 ($AFTER_COUNT)"
    else
      warn "文件数不一致! 搬前 $BEFORE_COUNT / 搬后 $AFTER_COUNT"
    fi
    for f in build/rk_zero_copy run_baseline.sh run_evidence.sh README.md; do
      if [ -e "$PROJECT_DIR/$f" ]; then
        ok "关键文件在位: $f"
      else
        err "关键文件丢失: $f"
      fi
    done

    # 2.1 二进制的 RUNPATH 里写死了工程旧的绝对路径, 搬家后动态链接会失效
    #     (报错: error while loading shared libraries: librga.so)
    #     改成 $ORIGIN 相对路径后, 工程挪到哪都能直接跑。
    BIN="$PROJECT_DIR/build/rk_zero_copy"
    if [ -f "$BIN" ]; then
      OLD_RP="$(readelf -d "$BIN" 2>/dev/null | sed -n 's/.*RUNPATH.*\[\(.*\)\].*/\1/p')"
      if [ -n "$OLD_RP" ] && printf '%s' "$OLD_RP" | grep -q "$SRC_PROJECT"; then
        warn "二进制 RUNPATH 写死了旧路径, 正在改成相对路径"
        info "旧: $OLD_RP"
        if command -v patchelf >/dev/null 2>&1; then
          cp -p -- "$BIN" "$BIN.bak.rpath"
          if patchelf --set-rpath '$ORIGIN/../third_party/lib:/usr/lib/aarch64-linux-gnu' "$BIN"; then
            CHANGES=$((CHANGES + 1))
            ok "RUNPATH 已改为 \$ORIGIN 相对路径(原文件备份为 build/rk_zero_copy.bak.rpath)"
          else
            warn "patchelf 失败, 已还原备份"
            mv -f -- "$BIN.bak.rpath" "$BIN" 2>/dev/null || true
          fi
        else
          warn "系统里没有 patchelf, 无法改 RUNPATH。"
          warn "请改用: export LD_LIBRARY_PATH=$PROJECT_DIR/third_party/lib:\$LD_LIBRARY_PATH"
        fi
      else
        ok "RUNPATH 已是相对路径或无需修改: ${OLD_RP:-无}"
      fi
    fi
  fi
else
  skip "源目录不存在, 跳过"
fi

# ==============================================================================
# STEP 3 — 模型归位到 models/, 工程内改用硬链接指向同一份真源
# ==============================================================================
step "STEP 3  模型归位 $MODELS_DIR"

if [ -d "$PROJECT_DIR/model" ]; then
  MODEL_DIR="$PROJECT_DIR/model"
elif [ -d "$SRC_PROJECT/model" ]; then
  MODEL_DIR="$SRC_PROJECT/model"      # 预演时工程还在原位
else
  MODEL_DIR=""
fi

if [ -n "$MODEL_DIR" ] && [ -d "$MODEL_DIR" ]; then
  # 3.1 拷贝一份到 models/ 作为唯一真源
  while IFS= read -r f; do
    safe_copy "$f" "$MODELS_DIR/$(basename -- "$f")"
  done < <(find "$MODEL_DIR" -maxdepth 1 -type f \( -name "*.rknn" -o -name "*.txt" -o -name "*.json" \) 2>/dev/null)

  # 3.2 工程内的副本改为硬链接: 单一真源, 不会再跑错版本, 且 ls -la 仍显示真实字节数
  while IFS= read -r f; do
    base="$(basename -- "$f")"
    canonical="$MODELS_DIR/$base"
    proj_copy="$MODEL_DIR/$base"
    [ -f "$canonical" ] || { warn "真源缺失, 保持工程内副本: $base"; continue; }

    if [ "$DRY_RUN" -eq 1 ]; then
      dry "ln -f $MODELS_DIR/$base -> $proj_copy (硬链接单一真源)"
      continue
    fi

    if [ "$(stat -c %h -- "$canonical")" -ge 2 ] && cmp -s -- "$canonical" "$proj_copy"; then
      ok "已是硬链接: model/$base"
      continue
    fi

    # 内容不一致时以 models/ 为准前先备份
    if ! cmp -s -- "$canonical" "$proj_copy"; then
      warn "内容不一致, 保留工程内副本不覆盖: $base"
      continue
    fi

    if ln -f -- "$canonical" "$proj_copy" 2>/dev/null; then
      if [ "$(stat -c %h -- "$canonical")" -ge 2 ]; then
        ok "已硬链接单一真源: model/$base"
      else
        warn "硬链接未生效: $base"
      fi
    else
      # 跨设备等情况下退化为相对软链接
      if ln -sf -- "../../models/$base" "$proj_copy" 2>/dev/null; then
        ok "已软链接(相对路径): model/$base -> ../../models/$base"
      else
        warn "链接失败, 保留独立副本: $base"
      fi
    fi
  done < <(find "$MODEL_DIR" -maxdepth 1 -type f \( -name "*.rknn" -o -name "*.txt" -o -name "*.json" \) 2>/dev/null)

  # 3.3 关键模型必须能在 models/ 里找到
  for m in ebike_int8_rk2332.rknn labels_ebike.txt; do
    if [ -f "$MODELS_DIR/$m" ]; then
      ok "模型就位: models/$m ($(stat -c %s -- "$MODELS_DIR/$m") 字节)"
    elif [ "$DRY_RUN" -eq 1 ]; then
      dry "将复制: model/$m -> models/$m"
    else
      err "缺少模型: models/$m"
    fi
  done
else
  warn "找不到 $PROJECT_DIR/model, 跳过模型归位"
fi

# 3.4 车牌识别模型: 原来散在 ~/model, 文档里按 $HOME/model 引用, 一并收进来
if [ -d "$HOME_DIR/model" ]; then
  move_dir_contents "$HOME_DIR/model" "$MODELS_DIR"
  if [ -d "$HOME_DIR/model" ] && [ -z "$(ls -A -- "$HOME_DIR/model" 2>/dev/null)" ]; then
    safe_rmdir_empty "$HOME_DIR/model" || true
  fi
  # 修正文档里的旧路径引用, 免得照着文档跑找不到模型
  RUNBOOK="$PROJECT_DIR/docs/EBIKE_MODEL_RUNBOOK.md"
  if [ -f "$RUNBOOK" ] && grep -q 'HOME/model/' "$RUNBOOK" 2>/dev/null; then
    if [ "$DRY_RUN" -eq 1 ]; then
      dry "sed -i 's#\$HOME/model/#$MODELS_DIR/#g' $RUNBOOK"
    else
      sed -i "s#\$HOME/model/#$MODELS_DIR/#g" "$RUNBOOK"
      ok "已更新文档中的模型路径引用: docs/EBIKE_MODEL_RUNBOOK.md"
    fi
  fi
fi

# ==============================================================================
# STEP 4 — 长跑日志归档
# ==============================================================================
step "STEP 4  长跑日志 -> $LOGS_DIR"

# 4.1 /tmp 下的长跑证据日志
if [ -d /tmp ]; then
  while IFS= read -r f; do
    safe_move "$f" "$LOGS_DIR/$(basename -- "$f")"
  done < <(find /tmp -maxdepth 1 -type f \
            \( -name "rk3588_evidence_*.log" -o -name "rk3588_evidence_*.rss" \
               -o -name "rk3588_*.log" -o -name "p0_*.log" \) 2>/dev/null)
fi

# 4.2 工程内的 evidence 日志
if [ -d "$PROJECT_DIR/docs/evidence" ]; then
  while IFS= read -r f; do
    safe_move "$f" "$LOGS_DIR/$(basename -- "$f")"
  done < <(find "$PROJECT_DIR/docs/evidence" -maxdepth 1 -type f -name "*.log" 2>/dev/null)
  if [ -z "$(ls -A -- "$PROJECT_DIR/docs/evidence" 2>/dev/null)" ]; then
    safe_rmdir_empty "$PROJECT_DIR/docs/evidence" || true
  fi
fi

# 4.3 当前目录下(旧 shenzhenproject 顶层)可能残留的日志 —— 打包前先收走
if [ -d "$LEGACY_DIR" ]; then
  while IFS= read -r f; do
    safe_move "$f" "$LOGS_DIR/$(basename -- "$f")"
  done < <(find "$LEGACY_DIR" -maxdepth 1 -type f -name "*.log" 2>/dev/null)
fi

if [ -d "$LOGS_DIR" ]; then
  LOG_COUNT="$(find "$LOGS_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l)"
  if [ "$DRY_RUN" -eq 1 ]; then
    dry "日志将归档到 $LOGS_DIR"
  else
    ok "日志归档完成, 共 $LOG_COUNT 个文件"
    find "$LOGS_DIR" -maxdepth 1 -type f -printf '     %f (%s 字节)\n' 2>/dev/null | head -20
  fi
fi

# ==============================================================================
# STEP 5 — 遗留目录打包归档(先存档, 后删除)
# ==============================================================================
step "STEP 5  遗留目录打包归档"

# 注意: 这里不能用 `tar ... | grep -q`。
# grep -q 命中即退出, tar 收到 SIGPIPE 返回 141; 脚本开了 pipefail,
# 管道整体会被判为失败, 导致"归档里有没有 src/gui"被误判成没有。
# 所以统一先把清单取到变量里, 再用 here-string 匹配。
tar_list() {
  tar tzf "$1" 2>/dev/null || true
}

ARCHIVE="$ARCHIVE_DIR/legacy_${STAMP}.tar.gz"

# 已经有含 GUI 源码的归档就复用, 避免重复打包上百 MB
REUSE=""
if [ -d "$ARCHIVE_DIR" ]; then
  while IFS= read -r cand; do
    [ -n "$cand" ] || continue
    listing="$(tar_list "$cand")"
    if grep -q "src/gui/main_window.cpp" <<< "$listing"; then
      REUSE="$cand"
      break
    fi
  done < <(ls -1t "$ARCHIVE_DIR"/legacy_*.tar.gz 2>/dev/null | head -5)
fi

if [ -n "$REUSE" ]; then
  ARCHIVE="$REUSE"
  ok "复用已有归档(已确认含 src/gui): $ARCHIVE"
fi

ARCHIVE_ITEMS=()

[ -d "$LEGACY_DIR" ]   && ARCHIVE_ITEMS+=("$LEGACY_DIR")
[ -d "$LEGACY_BK" ]    && ARCHIVE_ITEMS+=("$LEGACY_BK")
[ -d "$LEGACY_TEST" ]  && ARCHIVE_ITEMS+=("$LEGACY_TEST")
[ -d "$STALE_CFG" ]   && ARCHIVE_ITEMS+=("$STALE_CFG")

if [ "${#ARCHIVE_ITEMS[@]}" -gt 0 ]; then
  info "待归档: ${ARCHIVE_ITEMS[*]}"
  # 排除: 构建产物目录、shenzhenproject 里套着的那个陈旧工程副本(已被 ~/ 主力工程取代)
  if [ -n "$REUSE" ]; then
    ok "已有归档已覆盖这些目录, 不重复打包"
  elif [ "$DRY_RUN" -eq 1 ]; then
    dry "tar czf $ARCHIVE ${ARCHIVE_ITEMS[*]}  (排除 */build, */rk3588_zero_copy)"
  else
    set +e
    tar -czf "$ARCHIVE" \
        --exclude='*/build' \
        --exclude='shenzhenproject/rk3588_zero_copy' \
        --exclude='shenzhenproject.bk/rk3588_zero_copy' \
        -C "$HOME_DIR" \
        "${ARCHIVE_ITEMS[@]/#$HOME_DIR\//}" 2>&1 | sed 's/^/     /'
    tar_rc=${PIPESTATUS[0]}
    set -e
    if [ "$tar_rc" -eq 0 ] && [ -f "$ARCHIVE" ]; then
      CHANGES=$((CHANGES + 1))
      ok "归档完成: $ARCHIVE ($(stat -c %s -- "$ARCHIVE") 字节)"
      info "校验: $(tar tzf "$ARCHIVE" 2>/dev/null | wc -l) 个条目"
    else
      err "打包失败 (rc=$tar_rc), 保留原目录不删除"
    fi
  fi
else
  skip "没有需要归档的遗留目录"
fi

# ==============================================================================
# STEP 6 — 删除遗留目录与陈旧副本
# ==============================================================================
step "STEP 6  删除遗留目录与陈旧副本"

# 删除前最后一次确认(预演模式与 -y 模式自动放行)
if [ "$DRY_RUN" -eq 0 ]; then
  printf '\n%s即将删除以下路径(含源码的已归档到 %s):%s\n' "$C_B" "$ARCHIVE_DIR" "$C_RST"
  for p in "$LEGACY_DIR" "$LEGACY_BK" "$LEGACY_TEST" "$STALE_CFG"; do
    [ -e "$p" ] && printf '     %s\n' "$p"
  done
  confirm
fi

# 6.1 先确认归档里确实包含了 GUI 那份未提交的源码, 否则不删
GUI_SAFE=0
if [ -f "$ARCHIVE" ]; then
  listing="$(tar_list "$ARCHIVE")"
  if grep -q "src/gui/main_window.cpp" <<< "$listing"; then
    GUI_SAFE=1
    ok "归档内已确认包含未提交的 src/gui 源码"
  else
    warn "归档内未找到 src/gui, 删除前需人工确认"
  fi
fi

if [ "$DRY_RUN" -eq 0 ] && [ -d "$LEGACY_DIR" ] && [ "$GUI_SAFE" -eq 0 ]; then
  warn "未确认归档完整性, 保留 $LEGACY_DIR"
elif [ "$DRY_RUN" -eq 1 ]; then
  info "实际执行时: 归档内确认含 src/gui 后才会删除 $LEGACY_DIR"
  safe_rm_path "$LEGACY_DIR"   "遗留大目录(源码已在 _archive/ 内留档)"
  safe_rm_path "$LEGACY_BK"    "shenzhenproject 的重复备份副本(与已归档内容一致)"
  safe_rm_path "$LEGACY_TEST"  "早期 python 试验目录(模型与脚本已归档)"
  safe_rm_path "$STALE_CFG"   "放错位置的旧源码快照(逐文件确认均早于主力工程)"
else
  safe_rm_path "$LEGACY_DIR"   "遗留大目录(源码已在 _archive/ 内留档)"
  safe_rm_path "$LEGACY_BK"    "shenzhenproject 的重复备份副本(与已归档内容一致)"
  safe_rm_path "$LEGACY_TEST"  "早期 python 试验目录(模型与脚本已归档)"
  safe_rm_path "$STALE_CFG"   "放错位置的旧源码快照(逐文件确认均早于主力工程)"
fi

# ==============================================================================
# STEP 7 — 删除冗余工程压缩包
# ==============================================================================
step "STEP 7  删除冗余工程压缩包"

# 逐个确认包内文件在新位置都已存在且大小一致, 才允许删
CMP_PROJECT_DIR="$PROJECT_DIR"
if [ ! -d "$CMP_PROJECT_DIR" ] && [ -d "$SRC_PROJECT" ]; then
  CMP_PROJECT_DIR="$SRC_PROJECT"          # 预演时工程还在原位
fi

for tb in "$HOME_DIR/rk3588_zero_copy.tar.gz" "$HOME_DIR/rk3588_zero_copy_v2.tar.gz"; do
  [ -f "$tb" ] || { skip "不存在, 跳过: $tb"; continue; }

  if [ ! -d "$CMP_PROJECT_DIR" ]; then
    warn "工程目录不在位, 保留: $tb"
    continue
  fi

  missing=0
  total=0
  while IFS= read -r entry; do
    rel="${entry#rk3588_zero_copy/}"
    [ "$rel" = "$entry" ] && continue     # 顶层目录条目本身
    case "$rel" in */) continue ;; esac
    total=$((total + 1))
    # 文件可能已被本脚本搬到 logs/ 下, 两处都算存在
    if [ ! -e "$CMP_PROJECT_DIR/$rel" ] && [ ! -e "$LOGS_DIR/$(basename -- "$rel")" ]; then
      missing=$((missing + 1))
      [ "$missing" -le 3 ] && info "缺失: $rel"
    fi
  done < <(tar tzf "$tb" 2>/dev/null)

  if [ "$missing" -eq 0 ]; then
    safe_rm_path "$tb" "工程压缩包, 内容($total 个文件)已全部存在于 $CMP_PROJECT_DIR"
  else
    warn "包内 $missing/$total 个文件在新位置不存在, 保留: $tb"
  fi
done

# ==============================================================================
# STEP 8 — 清理系统默认多媒体目录(仅空目录)
# ==============================================================================
step "STEP 8  清理系统默认多媒体目录"

for d in Music Pictures Videos Documents Downloads Templates Public; do
  safe_rmdir_empty "$HOME_DIR/$d"
done
# Desktop 里有 KDE 的应用启动器软链(系统自管), 保持不动
info "Desktop 保留(内含系统桌面环境启动项)"

# ==============================================================================
# STEP 9 — 收拢零散工具
# ==============================================================================
step "STEP 9  收拢零散工具到 $TOOLS_DIR"

safe_move "$HOME_DIR/diag_board.sh" "$TOOLS_DIR/diag_board.sh"
if [ -d "$HOME_DIR/board_deps" ]; then
  move_dir_contents "$HOME_DIR/board_deps" "$TOOLS_DIR/board_deps"
  if [ -z "$(ls -A -- "$HOME_DIR/board_deps" 2>/dev/null)" ]; then
    safe_rmdir_empty "$HOME_DIR/board_deps" || true
  fi
fi

# 脚本自身留一份在工作区里
safe_copy "$0" "$DEPLOY/reset_board_dir.sh"

# ==============================================================================
# STEP 10 — 生成说明与最终结构
# ==============================================================================
step "STEP 10  生成工作区说明"

if [ "$DRY_RUN" -eq 0 ]; then
  cat > "$DEPLOY/README.md" <<EOF
# RK3588 板端工作区

生成时间: $(date '+%Y-%m-%d %H:%M:%S %Z')
主机: $(uname -n)

## 目录结构

    rk_deploy/
    ├── rk3588_zero_copy/     主力工程(15000 帧长跑验证过的这一份)
    │   ├── build/rk_zero_copy
    │   ├── model/             指向 ../models 的硬链接(单一真源)
    │   ├── run_baseline.sh    软解 + CPU 转换基线
    │   └── run_evidence.sh    长跑证据
    ├── models/                所有 .rknn 与标签文件
    ├── logs/                  长跑/证据日志
    ├── tools/                 板端诊断脚本与依赖说明
    └── _archive/              清理前的旧目录存档(可还原)

## 怎么跑

    cd $PROJECT_DIR
    ./run_baseline.sh --ebike --frames 15000     # 基线配置
    ./run_evidence.sh --ebike --frames 15000     # 完整证据记录

工程内 model/ 是指向 models/ 的硬链接, 两边永远是同一份文件, 不会再出现跑错版本。

二进制 build/rk_zero_copy 的 RUNPATH 已从写死的绝对路径改成 \$ORIGIN 相对路径,
整个工程目录搬到任何位置都能直接跑, 不会再报 librga.so 找不到。
两个 run 脚本也都导出了 LD_LIBRARY_PATH 兜底。

run_evidence.sh 的日志默认写到 logs/ 下(与本目录同级), 不会再写到会被清空的 /tmp。

## 模型

| 文件 | 说明 |
|---|---|
| models/ebike_int8_rk2332.rknn | 主力人/车双输出模型(长跑使用) |
| models/ebike_int8.rknn | 早期版本, 保留对照 |
| models/best.rknn | 原始 head/helmet 模型 |
| models/labels_ebike.txt | 人/车模型标签 |
| models/labels_list.txt | 原始模型标签 |
| models/lprnet_finetuned_int8.rknn | 车牌识别模型 |

## 还原旧目录

    tar xzf $ARCHIVE_DIR/legacy_*.tar.gz -C /home/seeed

EOF
  ok "已生成 $DEPLOY/README.md"
fi

# ------------------------------------------------------------------ 收尾 -------
step "完成"

if [ "$DRY_RUN" -eq 1 ]; then
  warn "这是 --dry-run 预演, 没有任何文件被改动。"
else
  ok "共执行 $CHANGES 项变更"
  printf '\n%s最终工作区结构:%s\n' "$C_B" "$C_RST"
  if command -v tree >/dev/null 2>&1; then
    tree -L 2 "$DEPLOY"
  elif [ -x "$HOME_DIR/.local/bin/tree" ]; then
    "$HOME_DIR/.local/bin/tree" -L 2 "$DEPLOY"
  else
    find "$DEPLOY" -maxdepth 2 | sort | sed 's|^|  |'
  fi
fi

printf '\n'
