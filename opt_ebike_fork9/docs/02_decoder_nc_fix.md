# 02 · 回归工具的 nc 写死问题(必须先修, 否则 ebike 的 fork9 回归根本跑不对)

## 一、问题

`accuracy/acc_batch.cc` 的 9 输出分支把类别数**写死成 80**:

```cpp
// accuracy/acc_batch.cc:318-322
if (io_num.n_output == 9) {
    // 标准 ultralytics 导出: 用自带解码器(DFL 距离要乘 stride)
    boxes = decode_9out_standard(ts, conf, nms, 80, img.cols, img.rows,
                                 model_w, model_h);
}
```

而 `decode_9out_standard` 内部是这样读 cls 的:

```cpp
for (int c = 0; c < nc; ++c) {
    const float v = ct.at(static_cast<uint32_t>(c) * grid + off);   // ★ c 越界
    ...
}
```

⇒ 对 **nc=2 的 ebike fork9 模型**, 它会去读 `c = 0..79`, 而 cls 张量只有 2 个通道
(`ct.at()` 读的是 `data + c*grid*4` 字节处) ⇒ **越界读**, 拿到的是内存里相邻的
box 张量/其他输出 ⇒ 分数、类别全是垃圾。

**上一阶段(task2)没踩到这个坑, 因为那一版的 fork9 模型是 COCO 的 nc=80。**
一旦要回归 ebike 的 nc=2 模型, 这条路是**直接走不通**的。

> 生产代码 `rk3588_zero_copy/src/yolo_post.cc` 是对的 —— 它从 cls 张量维度
> 自动推导 nc(`nc = outs[1].dims[n_dims-3]`, yolo_post.cc:591-593)。
> **只有回归工具 `accuracy/acc_batch.cc` 写死了。**

## 二、修法(沙盒副本, 不动共享文件)

`opt_ebike_fork9/src/acc_batch_nc.cc` —— `accuracy/acc_batch.cc` 的副本, 只改这一处:

```cpp
if (io_num.n_output == 9) {
    // ★★ nc 必须从 cls 张量自动推导(形态B: cls 是 [1, nc, H, W]
    //    -> nc = dims[n_dims-3]), 与生产 src/yolo_post.cc 的做法一致。
    //    原来这里写死 80(COCO 口径): 对 nc=2 的 ebike fork9 模型,
    //    decode_9out_standard 会按 c=0..79 去读只有 2 个通道的 cls 张量
    //    -> 越界读 -> 垃圾框 / 可能直接崩。
    int nc9 = 80;
    if (ts.size() > 1 && ts[1].n_dims >= 3) {
        nc9 = static_cast<int>(ts[1].dims[ts[1].n_dims - 3]);
    }
    boxes = decode_9out_standard(ts, conf, nms, nc9, img.cols, img.rows,
                                 model_w, model_h);
}
```

编译(板端):

```bash
R=/home/seeed/rk_deploy/rk3588_zero_copy
g++ -O2 -std=c++17 -o src/acc_batch_nc src/acc_batch_nc.cc \
    $R/src/lprnet.cc $R/src/license_plate_rec.cc $R/src/yolo_post.cc \
    -I$R/include -I$R/third_party/include -I$R/third_party/include/rga \
    $R/third_party/lib/librknnrt.so $R/third_party/lib/librga.so \
    $(pkg-config --cflags --libs opencv4)
```

## 三、验证: 这个副本是"行为等价"的, 不是"顺手改的"

### 验证① nc=80 的 fork9 模型上, 与仓库原版**逐字节一致**

```bash
cd /home/seeed/rk_deploy/accuracy
export LD_LIBRARY_PATH=/home/seeed/rk_deploy/rk3588_zero_copy/third_party/lib
head -120 data/manifests/frames_list.txt > /tmp/frames120.txt
./acc_batch yolo --model .../opt_dfl/models/armB_fork9_dfl_on_cpu_int8.rknn \
    --list /tmp/frames120.txt --out /tmp/boxes_stock_nc80.csv --conf 0.25 --nms 0.45
../opt_ebike_fork9/src/acc_batch_nc yolo --model .../armB_fork9_dfl_on_cpu_int8.rknn \
    --list /tmp/frames120.txt --out /tmp/boxes_ncauto_nc80.csv --conf 0.25 --nms 0.45
diff /tmp/boxes_stock_nc80.csv /tmp/boxes_ncauto_nc80.csv
```

```
IDENTICAL ✔ (逐字节一致)
3272622a71fb57aca214ab195403cf8f  /tmp/boxes_stock_nc80.csv
3272622a71fb57aca214ab195403cf8f  /tmp/boxes_ncauto_nc80.csv
```

⇒ 对 nc=80, 新旧解码器**行为完全相同**(md5 一致), 唯一的变化就是 nc 这个数。

### 验证② 形态C(2 输出)路径完全没被碰

用生产 ebike 模型跑 1724 帧, 和仓库里 2026-11-13 那份 `accuracy/out/boxes_ebike_int8.csv` 比:

```
IDENTICAL ✔
```

⇒ 2 输出(形态C)路径一行没改, 结果逐字节相同。

## 四、结论

* **回归工具现在能正确处理 nc=2 的 fork9 模型了。**
* 这个修改**只存在于沙盒副本** `opt_ebike_fork9/src/acc_batch_nc.cc`,
  `accuracy/acc_batch.cc` **一个字节都没动** —— 符合"不改生产/共享代码"的红线。
* 建议: 等 ebike fork9 真的要上生产时, 把这 3 行改动**合并回** `accuracy/acc_batch.cc`
  (它不是生产推理代码, 是回归工具), 否则以后谁拿 nc≠80 的 fork9 模型做回归都会踩坑。
