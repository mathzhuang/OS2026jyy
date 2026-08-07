# Labyrinth 迷宫游戏实验报告

## 实验概述

Labyrinth 是一个基于命令行的迷宫游戏实现，属于南京大学蒋炎岩老师操作系统（OS/2026）课程的 M1 实验。本实验的目标是通过实现一个完整的迷宫管理和玩家移动系统，深入理解文件 I/O、数据结构和算法的应用。

## 实验目标

1. 实现命令行参数解析
2. 实现文件的读写操作
3. 实现数据结构的设计与管理
4. 实现图的连通性检查算法
5. 实现玩家的移动和状态管理

## 核心功能实现

### 1. 命令行参数解析

程序支持以下命令行选项：

- `--version` / `-v`: 显示版本信息
- `--map <file>` / `-m <file>`: 指定迷宫地图文件
- `--player <id>` / `-p <id>`: 指定玩家 ID（0-9）
- `--move <direction>`: 指定移动方向（up/down/left/right）

**示例用法：**

```bash
./labyrinth --version
./labyrinth --map map.txt --player 1
./labyrinth -m map.txt -p 1 --move right
```

### 2. 地图文件格式

地图文件是一个矩形网格，由以下字符组成：

- `.`: 表示空地（玩家可以走过）
- `#`: 表示墙（玩家不能穿过）
- `0-9`: 表示玩家 ID

地图必须满足以下条件：

1. 所有行的长度必须相同（矩形）
2. 所有行的列数不能超过 MAX_COLS (100)
3. 所有的空地必须连通（即所有 `.` 字符必须相互可达）

**示例地图：**

```
###
#1.
###
```

### 3. 核心函数实现

#### isValidPlayer(char playerId)

验证玩家 ID 的合法性，有效的玩家 ID 是 0-9 的数字。

```c
bool isValidPlayer(char playerId) {
    return playerId >= '0' && playerId <= '9';
}
```

#### loadMap(Labyrinth *labyrinth, const char *filename)

从文件中加载迷宫地图。

- 打开文件并逐行读取
- 验证所有行的长度是否一致
- 检查行数和列数是否超过限制
- 失败情况：无法打开文件、行长度不一致、超过限制

```c
bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (f == NULL) return false;
    
    labyrinth->rows = 0;
    labyrinth->cols = -1;
    
    // 逐行读取并验证...
}
```

#### findPlayer(Labyrinth *labyrinth, char playerId)

在迷宫中查找指定的玩家。

- 按行优先顺序遍历迷宫
- 返回玩家的位置，未找到则返回 (-1, -1)

#### findFirstEmptySpace(Labyrinth *labyrinth)

查找迷宫中第一个空地。

- 按行优先顺序查找 `.` 字符
- 用于玩家不存在时的生成位置

#### isEmptySpace(Labyrinth *labyrinth, int row, int col)

检查指定位置是否为空地。

- 验证坐标是否在范围内
- 检查该位置是否为 `.` 字符

#### movePlayer(Labyrinth *labyrinth, char playerId, const char *direction)

移动玩家到指定方向。

**核心逻辑：**

1. 验证方向参数（up/down/left/right）
2. 查找玩家位置
3. 如果玩家不存在，在第一个空地处生成
4. 计算目标位置
5. 检查目标位置是否为空地
6. 清除旧位置，更新新位置

**失败情况：**
- 无效的方向
- 玩家要移动到墙或其他玩家的位置
- 没有空地用于生成新玩家

#### saveMap(Labyrinth *labyrinth, const char *filename)

将迷宫地图保存到文件。

- 打开文件用于写入
- 逐行写入地图，每行后添加换行符

#### isConnected(Labyrinth *labyrinth)

检查迷宫的所有空地是否连通。

**算法：深度优先搜索 (DFS)**

1. 初始化访问标记数组
2. 从第一个空地开始进行 DFS
3. 遍历所有空地，检查是否都被访问过
4. 如果有未访问的空地，返回 false；否则返回 true

**四方向连通性检查：**
- 上：(row-1, col)
- 下：(row+1, col)
- 左：(row, col-1)
- 右：(row, col+1)

```c
void dfs(Labyrinth *labyrinth, int row, int col, 
         bool visited[MAX_ROWS][MAX_COLS]) {
    // 边界检查
    if (row < 0 || row >= labyrinth->rows || 
        col < 0 || col >= labyrinth->cols) {
        return;
    }
    
    // 已访问或为墙
    if (visited[row][col] || labyrinth->map[row][col] == '#') {
        return;
    }
    
    visited[row][col] = true;
    
    // 递归访问四个方向
    dfs(labyrinth, row - 1, col, visited);
    dfs(labyrinth, row + 1, col, visited);
    dfs(labyrinth, row, col - 1, visited);
    dfs(labyrinth, row, col + 1, visited);
}
```

## 主程序流程

```
1. 解析命令行参数
   ├─ 处理 --version: 打印版本信息并退出
   └─ 验证必需参数：--map 和 --player
   
2. 验证玩家 ID
   └─ 必须是 0-9 的数字

3. 加载地图
   └─ 验证文件格式和行长度一致性

4. 检查连通性
   └─ 确保所有空地连通

5. 根据是否提供 --move 参数执行不同操作
   ├─ 有 --move：移动玩家，保存地图
   └─ 无 --move：打印当前地图
```

## 错误处理

程序会在以下情况返回错误代码 (1)：

- 无效的命令行参数
- 无效的玩家 ID
- 无法打开或读取地图文件
- 地图格式错误（行长度不一致）
- 地图不连通
- 无效的移动方向
- 玩家移动被阻挡（撞墙或碰到其他玩家）

成功的操作返回 0。

## 数据结构

### Labyrinth 结构体

```c
typedef struct {
    char map[MAX_ROWS][MAX_COLS];  // 二维地图数组
    int rows;                       // 行数
    int cols;                       // 列数
} Labyrinth;
```

### Position 结构体

```c
typedef struct {
    int row;  // 行坐标
    int col;  // 列坐标
} Position;
```

## 测试用例说明

程序实现了完整的单元测试和系统测试，包括：

**单元测试：**
- 玩家 ID 验证测试
- 空地检测测试
- 玩家查找测试
- 第一个空地查找测试
- 地图连通性测试

**系统测试：**
- 版本信息显示
- 参数解析
- 地图打印
- 玩家移动和持久化
- 玩家生成与移动
- 移动被阻挡
- 无效方向处理
- 各种错误情况

## 重要实现细节

### 玩家生成机制

当指定的玩家 ID 在地图中不存在时，程序会：

1. 自动在第一个空地处生成该玩家
2. 然后立即执行指定的移动

这使得多个玩家可以被逐次创建并移动。

### 地图打印

当不提供 `--move` 参数时，程序会原样打印当前地图，包括所有玩家和障碍物。

### 文件持久化

成功的移动操作会立即将更新后的地图写回到文件中，确保游戏状态被保存。

## 编译和运行

```bash
# 编译
cd labyrinth
make

# 运行示例
./labyrinth --version
./labyrinth --map maps/map.txt --player 1
./labyrinth --map maps/map.txt --player 1 --move right

# 清理编译文件
make clean
```

## 总结

本实验通过实现一个完整的迷宫游戏系统，涵盖了以下关键知识点：

1. **文件 I/O**：文件的读写操作和错误处理
2. **字符串处理**：命令行参数解析和字符串操作
3. **数据结构**：使用结构体管理复杂数据
4. **算法**：深度优先搜索用于连通性检查
5. **系统设计**：完整的程序流程设计和错误处理

这个实验体现了操作系统编程中对细节的要求和对系统级编程的理解。
