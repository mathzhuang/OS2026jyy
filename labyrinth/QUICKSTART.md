# M1 Labyrinth - 快速开始指南

## 编译

```bash
cd labyrinth
make
```

## 基本命令

### 版本信息
```bash
./labyrinth --version
# 输出: Labyrinth Game
```

### 打印地图
```bash
./labyrinth --map <地图文件> --player <玩家ID>
# 示例:
./labyrinth --map map.txt --player 1
```

### 移动玩家
```bash
./labyrinth --map <地图文件> --player <玩家ID> --move <方向>
# 示例:
./labyrinth --map map.txt --player 1 --move right  # 向右移动
./labyrinth --map map.txt --player 1 --move left   # 向左移动
./labyrinth --map map.txt --player 1 --move up     # 向上移动
./labyrinth --map map.txt --player 1 --move down   # 向下移动
```

## 测试

### 运行所有测试（22个测试用例）
```bash
TK_RUN=1 ./labyrinth
```

### 显示测试详细输出
```bash
TK_VERBOSE=1 TK_RUN=1 ./labyrinth
```

## 地图文件格式

- `.` 表示空地（可以走）
- `#` 表示墙（不能走）
- `0-9` 表示玩家（玩家1-9）

示例：
```
###
#1.
###
```

## 返回码

- `0` - 成功
- `1` - 失败（参数错误、地图问题、移动失败等）

## 常见错误

| 错误信息 | 原因 | 解决方案 |
|--------|------|--------|
| 无输出，返回码1 | 缺少参数 | 提供 --map 和 --player 参数 |
| 无输出，返回码1 | 玩家ID无效 | 玩家ID必须是 0-9 的数字 |
| 无输出，返回码1 | 地图不存在 | 检查文件路径 |
| 无输出，返回码1 | 地图行长度不一致 | 地图必须是矩形 |
| 无输出，返回码1 | 地图不连通 | 所有空地必须连通 |
| 无输出，返回码1 | 移动被阻挡 | 目标位置必须是空地 |

## 文档

- **REPORT.md** - 项目综合说明
- **DEVLOG.md** - 开发过程详细记录

## 清理

```bash
make clean
```
