# RK3588 板端工作区

生成时间: 2026-11-10 16:22:27 CST
主机: recomputer-rk3588-devkit

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

    cd /home/seeed/rk_deploy/rk3588_zero_copy
    ./run_baseline.sh --ebike --frames 15000     # 基线配置
    ./run_evidence.sh --ebike --frames 15000     # 完整证据记录

工程内 model/ 是指向 models/ 的硬链接, 两边永远是同一份文件, 不会再出现跑错版本。

二进制 build/rk_zero_copy 的 RUNPATH 已从写死的绝对路径改成 $ORIGIN 相对路径,
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

    tar xzf /home/seeed/rk_deploy/_archive/legacy_*.tar.gz -C /home/seeed

