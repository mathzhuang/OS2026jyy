#!/bin/bash
# 构造一棵深 WIDTH 叉、DEPTH 层的进程树，然后在树存活期间运行 pstree 观察形态。
#
# 用法:
#   ./test_tree.sh [DEPTH] [WIDTH] [HOLD秒]
# 默认:
#   DEPTH=3  WIDTH=3  HOLD=4   -> 3 层、每层 3 叉
#
# 说明:
#   本脚本自身跑在一个 bash 里，pstree 以该 bash 为根打印，
#   因此下面 spawn 出来的子进程都会作为根 bash 的后代出现。
#   运行前会自动编译 pstree.c（源码比二进制新，或二进制不存在时）。

DEPTH=${1:-3}
WIDTH=${2:-3}
HOLD=${3:-4}
export HOLD   # 让子 shell 也能看到叶子存活时长

# 自动编译：改了 pstree.c 后无需再手动 gcc
if [ ! -x ./pstree ] || [ pstree.c -nt ./pstree ]; then
    echo ">> 编译 pstree..."
    gcc -O2 -std=gnu2x -ggdb -Wall -o pstree pstree.c || {
        echo "!! 编译失败，终止" >&2
        exit 1
    }
fi

# 递归函数：在 d 层构建 w 叉子树
spawn() {
    local d=$1 w=$2
    if [ "$d" -le 0 ]; then
        sleep "$HOLD"          # 叶子：保持存活，让 pstree 能看到
        return
    fi
    local i
    for ((i = 0; i < w; i++)); do
        bash -c "$(declare -f spawn); spawn $((d - 1)) $w" &
    done
    wait                       # 等所有子节点结束再退出，保持整棵树存活
}

# 把整棵子树挂到后台，作为根 bash 的孩子
spawn "$DEPTH" "$WIDTH" &
TREE_PID=$!

sleep 0.5                      # 给树一点时间长起来

echo "=== pstree -p （按名称排序） ==="
./pstree -p

echo
echo "=== pstree -p -n （按 PID 排序） ==="
./pstree -p -n

wait "$TREE_PID"               # 等树自然结束，回收
