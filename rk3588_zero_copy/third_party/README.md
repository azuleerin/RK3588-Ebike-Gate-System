# third_party —— Rockchip 板端运行时

本目录存放本项目在 RK3588 上运行所需的 Rockchip 官方运行时二进制与头文件。

| 文件 | 说明 |
|---|---|
| `lib/librknnrt.so` | RKNN 运行时（模型加载与推理） |
| `lib/librga.so` | RGA 运行时（2D 图形加速，零拷贝预处理） |
| `include/rknn_api.h` | RKNN API 头文件 |
| `include/rga/` | RGA API 头文件 |

## 许可说明

> ⚠️ **本目录中的文件不受仓库根目录的 MIT 协议覆盖。**
> 它们是 Rockchip Electronics 的专有运行时，版权归 Rockchip 所有，
> 按 Rockchip EULA 分发，仅用于在 RK3588 硬件上运行本项目。
> 仓库根目录的 [MIT 协议](../../LICENSE) 只覆盖本项目自身的源代码与文档。

## 运行时版本约束

板端运行时版本必须与模型转换工具链版本匹配，否则模型加载会失败：

| 组件 | 版本 |
|---|---|
| 板端 RKNN 运行时 | **2.3.2** |
| 模型转换工具链 | **2.3.2** |

> `models/ebike_int8.rknn` 由 toolkit **2.5.0** 编译，在本板端运行时上**加载会失败**，
> 生产请用 `models/ebike_int8_rk2332.rknn`。

## 加载方式

二进制已通过 `$ORIGIN` 相对路径的 RUNPATH 定位依赖，整个工程目录搬到任何位置都能直接运行；
脚本另外兜底导出 `LD_LIBRARY_PATH`：

```bash
export LD_LIBRARY_PATH="$PWD/third_party/lib:$PWD/lib:/usr/lib:$LD_LIBRARY_PATH"
```
