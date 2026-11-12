board_deps/ 里是 RK3588(Ubuntu resolute / arm64) 的 .deb 安装包, 全部离线可用。

板子上执行:
    cd <包含 board_deps 的目录>
    sudo ./install_deps.sh board_deps

或者手动:
    sudo dpkg -i board_deps/*.deb
    sudo dpkg --configure -a
    sudo ldconfig

注意:
  * 板子【没有外网】, 所以不能用 apt-get -f install 去网上补依赖;
    本目录已经包含了完整依赖闭包, dpkg -i 一次装齐即可。
  * 全部是 arm64 架构, 不会被当成 x86 包。
  * ffmpeg 来自 Ubuntu 官方源, 【没有 h264_rkmpp 硬解码器】(Debian/Ubuntu 不带
    Rockchip MPP 支持)。硬解需要 Rockchip SDK 里的 ffmpeg-rockchip。
    没有硬解时本工程会自动退回软解码, 流水线照常跑。
  * MPP 的 .deb 来自 Radxa 的 apt 仓库, 只提供头文件(/usr/include/rockchip/)
    和库, 版本是 20190626(旧 API), 够本工程取 dma-buf fd 用了。
