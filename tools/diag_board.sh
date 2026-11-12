#!/bin/bash
echo '=== python3.12 ==='
python3.12 --version 2>&1
which python3.12 2>&1
echo '=== python3.12 import rknnlite ==='
python3.12 -c 'import rknnlite; from rknnlite.api import RKNNLite; print("rknnlite OK", rknnlite.__file__)' 2>&1
echo '=== rknnlite 版本与初始化 ==='
python3.12 -c 'from rknnlite.api import RKNNLite; r=RKNNLite(); print("RKNNLite init OK")' 2>&1
echo '=== numpy/cv2 in py3.12 ==='
python3.12 -c 'import numpy; print("numpy", numpy.__version__)' 2>&1
python3.12 -c 'import cv2; print("cv2", cv2.__version__)' 2>&1
echo '=== 板端 shenzhenproject 测试文件 ==='
ls -la /home/seeed/shenzhenproject/ 2>&1 | head -30
