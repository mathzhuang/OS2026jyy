#!/bin/bash

echo "=== 测试1: 版本显示 ==="
./labyrinth --version
echo "Exit code: $?"
echo

echo "=== 测试2: 版本后跟其他参数（应该失败）==="
./labyrinth --version ?? 
echo "Exit code: $?"
echo

echo "=== 测试3: 无参数（应该失败）==="
./labyrinth
echo "Exit code: $?"
echo

echo "=== 测试4: 创建测试地图 ==="
echo -n "###
#1.
###" > test.map
cat test.map
echo
echo

echo "=== 测试5: 打印地图 ==="
./labyrinth --map test.map --player 1
echo "Exit code: $?"
echo

echo "=== 测试6: 创建可移动的地图 ==="
echo -n "1.
.." > movable.map
echo "地图内容:"
cat movable.map
echo
echo

echo "=== 测试7: 玩家右移 ==="
./labyrinth --map movable.map --player 1 --move right
echo "Exit code: $?"
echo "地图更新后:"
cat movable.map
echo
echo

echo "=== 测试8: 创建不连通的地图 ==="
echo -n ".#.
###
.#." > disconnected.map
echo "地图内容:"
cat disconnected.map
echo
echo

echo "=== 测试9: 检查不连通地图（应该失败）==="
./labyrinth --map disconnected.map --player 1
echo "Exit code: $?"
echo

rm -f test.map movable.map disconnected.map
