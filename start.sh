#!/bin/bash

if [ $# -eq 0 ]; then
    # 没有参数时执行原有逻辑
    git reset --hard 910d59b6dc93b9cc26c9a0c27bc1cdc32be8b7db
    rm -rf ~/c-home/client
    rm -rf ~/c-home/FIFO_*
    rm -rf ~/c-home/server
    git pull origin dev
    # 编译并启动服务器
    make clean && make && ./server 1000
else
    # 有参数时只执行客户端连接
    cd ~/c-home && ./client $1 daniel
fi