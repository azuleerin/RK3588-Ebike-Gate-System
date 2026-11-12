# P0 板端执行手册 —— MPP 硬解 + 零拷贝

> 状态：Windows 侧已全部就绪。**只差人在板子上执行 3 条命令。**
> 板子：`192.168.31.77`（`seed`），已在网（ping 通、22 端口开），SSH 需密码 → 板端命令由人执行。
> 开发机：`192.168.31.214`（同网段），已起 HTTP 文件服务 `http://192.168.31.214:8900/`。

---

## 一、为什么只需要 3 条命令

交接文档 P0 的原始流程要传 149MB 的 `board_deps/` 再 `dpkg -i` 全部。
**本轮核实后不需要**：板子已经编译通过软解基线，说明工具链 / ffmpeg-dev / OpenCV-dev 都在，
真正缺的只有 Rockchip MPP 那两个包。而它们的依赖只有 `libc6 >= 2.17 / libgcc1 / libstdc++6`——
**不碰任何核心库**，装它不会把板子搞坏（原始 `dpkg -i` 全量反而有降级 `libc6` 搞挂 systemd 的风险）。

所以做法是：**只装 MPP 两个包**（383KB），其余由 `p0_mpp_verify.sh` 自适应处理。

---

## 二、板子上执行这 3 条（一条一条来）

### 第 1 条：拉工程包

```bash
cd $HOME && curl -fLO http://192.168.31.214:8900/rk3588_zero_copy_v2.tar.gz
```

> 板子没外网，所以是从**开发机**拉，不是从公网。
> 若板子上没有 `curl` 也没有 `wget`，改用 Windows 端 scp（会提示输密码）：
> ```powershell
> scp E:\shenzhenproject\rk3588_zero_copy_v2.tar.gz seeed@192.168.31.77:$HOME/
> ```

### 第 2 条：解压

```bash
cd $HOME && rm -rf rk3588_zero_copy && tar xzf rk3588_zero_copy_v2.tar.gz
```

### 第 3 条：一键验收

```bash
cd $HOME/rk3588_zero_copy && ./p0_mpp_verify.sh
```

脚本会依次做：体检 → 拉并安装 MPP 两个包 → CMake 配置 → 编译 → 跑 300 帧 →
断言验收 → 打印结论。**全程约 3~5 分钟**（编译占大头）。

> 脚本里 `sudo dpkg -i` 会提示输密码，属正常。

---

## 三、验收标准（脚本自动判定）

| # | 判定依据 | 通过的样子 |
|---|---|---|
| 1 | CMake 找到 MPP | `-- MPP        = /usr/lib/aarch64-linux-gnu/librockchip_mpp.so (header /usr/include/rockchip)` |
| 2 | 硬解后端生效 | `[解码] 后端 = Rockchip MPP 硬解 (H.264, VPU)` |
| 3 | 零拷贝生效 | `[NV12] 数据源: dma-buf fd (RGA import)` |
| 4 | 解码耗时 | 最后一行 `[frame N] ... dec=1.xxms`（软解一般 15~30ms） |

全通过时脚本最后打印：`✅ P0 通过: MPP 硬解 + dma-buf 零拷贝 都已生效`，退出码 0。

---

## 四、失败了怎么回传

脚本**不管成功失败都会**把诊断包打在屏幕上，同时存三个文件：

| 文件 | 内容 |
|---|---|
| `/tmp/p0_mpp_verify.log` | 全流程日志 |
| `/tmp/p0_cmake.log` | CMake 完整输出 |
| `/tmp/p0_run.log` | 运行日志（解码后端 / NV12 数据源 / 每帧统计） |

把 `/tmp/p0_mpp_verify.log` 的内容贴回来即可定位。

常见分支：

| 现象 | 原因 | 处理 |
|---|---|---|
| `MPP 头文件仍然缺失` | 装包失败 | 看 `dpkg -l \| grep rockchip-mpp`；多半是 `sudo` 没输对 |
| CMake 报 `找不到 OpenCV` | 板子只有 OpenCV 运行库没头文件 | 需要补 `libopencv-dev` |
| `[警告] MPP 初始化失败` | 内核没加载 rkvdec | `dmesg \| grep -iE "rkvdec\|vpu"`；可能要升级内核 |
| 硬解成功但 `NV12 数据源: CPU 映射地址` | dma-buf fd 没拿到 | 设 `RK_NO_MPP_FD_LOOKUP=0`；查 `/proc/self/maps` 反查路径 |
| 一帧都没出来 | RTSP 没连上 | 确认相机 `192.168.31.200:8557` 通 |

---

## 五、跑完之后

P0 通过后，下一步按交接文档走：

- **P1** 车牌 ROI 比例标定（需要实拍素材，板端 `best.rknn` 只有 head/helmet 两类，
  要换带车辆类的模型或用 `--plate-cls` 指定）
- **P2** MQTT 闭环：开发机 `1883` 已经在跑 broker，板端加
  `--mqtt-host 192.168.31.214 --mqtt-qos 1 --sink-stdout`

---

## 六、Windows 侧收尾

```powershell
# 关掉给板子用的 HTTP 文件服务(板子拿完包后)
#   在跑 python -m http.server 8900 的那个窗口按 Ctrl+C

# 重新打包(改过源码后)
cd E:\shenzhenproject && python regen_all.py

# 校验交付包
cd E:\shenzhenproject && tar xzf rk3588_zero_copy_v2.tar.gz -C <临时目录> && <临时目录>\rk3588_zero_copy\p0_mpp_verify.sh
```

## 七、本轮在 Windows 侧已经做完的事

1. **交付包完整性**：83 文件，干净目录解压后与源码逐字节一致；`CHECKSUMS.txt` 与包内容一一对应。
2. **修好可执行位**：Windows 的 `tarfile.add` 原本把 `.sh` 打成 `0666`，板端 `./xxx.sh` 会
   Permission denied。已在 `regen_all.py` 里加 `_tar_filter` 强制 `.sh/.ps1` 为 `0755`。
3. **补齐漏包的文件**：`build.sh`、`deploy.sh`、`docs/evidence/board_cpp_build.log`
   原先不在包里（交接文档 §2.1 却声称有 `build.sh`），已加入。
4. **MPP 链接期风险离线排除**：读真 `librockchip_mpp.so.0` 的 ELF 动态符号表 + 解包真头文件，
   逐项核对——16 个 MPP 符号全部命中，枚举/类型/`MppApi` 成员齐全，
   宏 `mpp_buffer_get_fd` → `mpp_buffer_get_fd_with_caller` 确实存在于符号表。
   **板端编译链接阶段不会再因 MPP 失败。**
5. **写好 `p0_mpp_verify.sh`**：体检 → 装包 → 配置 → 编译 → 运行 → 断言 → 诊断包，
   一条命令跑完全流程并给出 PASS/FAIL。
6. **修好 `download_deps.ps1` 的一个隐蔽 bug**：第 297 行 `$(math]::Round(...)` 少了 `[`，
   PowerShell 会把 `math]` 当命令调用，报的是 `op_Division: [System.Object[]] 不包含...`
   —— 报错指向除法，看不出跟方括号有关，会把排查带偏。已修，并让脚本**跳过已下好的包**（可重跑）。
7. **离线依赖齐了**：`board_deps/` 566 个 arm64 deb / 430MB，SHA256 **全部通过**，
   含 `librockchip-mpp1` + `librockchip-mpp-dev`（Radxa 源）。
   万一板端缺别的依赖，整个目录传过去 `sudo ./install_deps.sh board_deps` 即可。

## 八、这一轮用到的两个离线工具（仓库根目录）

| 工具 | 作用 |
|---|---|
| `deb_inspect.py` | 不依赖 dpkg-deb/ar/tar，直接看 .deb 的 control 字段和会装到哪些路径 |
| `elf_dynsym.py` | 不依赖 binutils，解析 ELF `.dynsym` 列导出符号（用来验 MPP 链接可行性） |

```powershell
python deb_inspect.py board_mpp\librockchip-mpp-dev_20190626-1_arm64.deb
python elf_dynsym.py <解出来的 librockchip_mpp.so.0> mpp_buffer_get_fd
```

