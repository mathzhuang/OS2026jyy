#!/bin/bash

# 创建初始地图
printf "1.\n..\n" > move_test.map

echo "初始地图："
cat move_test.map
echo "---"
echo

# 执行移动
echo "执行: ./labyrinth --map move_test.map --player 1 --move right"
./labyrinth --map move_test.map --player 1 --move right
echo "返回码: $?"
echo

echo "移动后地图内容："
cat move_test.map
echo "---"
echo

# 用xxd显示具体字节
echo "字节表示（xxd）："
xxd move_test.map

rm move_test.map
