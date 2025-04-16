#!/bin/bash

# 创建构建目录
mkdir -p build
cd build

# 配置并构建
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 安装可执行文件
make install

# 打印完成信息
echo "构建完成，可执行文件已生成在test目录中" 