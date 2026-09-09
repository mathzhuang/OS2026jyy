#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <testkit.h>
#include "labyrinth.h"

int main(int argc, char *argv[]) {
    // 定义变量来存储解析到的参数
    const char *map_file = NULL;
    char player_id = '\0';
    const char *move_dir = NULL;
    bool is_version = false;

    // 遍历 argv 数组解析参数 (argv[0] 是程序名，从 i = 1 开始)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            is_version = true;
            // --version 不能与其他参数混合
            if (argc > 2) {
                return 1;
            }
        } else if (strcmp(argv[i], "--map") == 0 || strcmp(argv[i], "-m") == 0) {
            // 处理 --map 或 -m 选项，获取下一个参数作为文件名
            if (i + 1 < argc) {
                map_file = argv[++i];
            } else {
                return 1;
            }
        } else if (strcmp(argv[i], "--player") == 0 || strcmp(argv[i], "-p") == 0) {
            // 处理 --player 或 -p 选项，获取下一个参数的第一个字符作为玩家 ID
            if (i + 1 < argc) {
                player_id = argv[++i][0];
            } else {
                return 1;
            }
        } else if (strcmp(argv[i], "--move") == 0) {
            // 处理 --move 选项，获取下一个参数作为移动方向
            if (i + 1 < argc) {
                move_dir = argv[++i];
            } else {
                return 1;
            }
        } else {
            // 未知参数，直接返回错误
            return 1;
        }
    }

    // 处理 --version 命令
    if (is_version) {
        printf("%s\n", VERSION_INFO);
        return 0;
    }

    // 非 --version 命令必须提供 --map 和 --player 参数
    if (map_file == NULL || player_id == '\0') {
        return 1;
    }

    // 验证玩家 ID 的合法性（应该是 0-9 的数字）
    if (!isValidPlayer(player_id)) {
        return 1;
    }

    // 加载迷宫地图
    Labyrinth lab;
    if (!loadMap(&lab, map_file)) {
        return 1;
    }

    // 验证地图连通性（所有空地必须连通）
    if (!isConnected(&lab)) {
        return 1;
    }

    // 判断是"打印地图"还是"移动玩家"
    if (move_dir != NULL) {
        // 存在 --move 参数：尝试移动玩家
        if (!movePlayer(&lab, player_id, move_dir)) {
            return 1;
        }
        // 移动成功，保存更新后的地图
        if (!saveMap(&lab, map_file)) {
            return 1;
        }
    } else {
        // 不存在 --move 参数：只打印地图
        for (int i = 0; i < lab.rows; i++) {
            printf("%s\n", lab.map[i]);
        }
    }

    return 0;
}

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

// 验证玩家 ID 的合法性
// 有效的玩家 ID 是 0-9 的数字
bool isValidPlayer(char playerId) {
    return playerId >= '0' && playerId <= '9';
}

// 从文件中加载迷宫地图
// 返回 true 表示加载成功，false 表示加载失败
// 失败的原因可能是：
//   1. 无法打开文件
//   2. 行数超过 MAX_ROWS
//   3. 列数超过 MAX_COLS
//   4. 行长度不一致（非矩形地图）
bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        return false;
    }

    labyrinth->rows = 0;
    labyrinth->cols = -1;

    char line[MAX_COLS + 2];
    while (fgets(line, sizeof(line), f) != NULL && labyrinth->rows < MAX_ROWS) {
        // 移除行尾的换行符
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        // 检查列数是否一致
        if (labyrinth->cols == -1) {
            labyrinth->cols = len;
        } else if (labyrinth->cols != len) {
            // 行长度不一致，地图格式错误
            fclose(f);
            return false;
        }

        // 检查列数是否超过限制
        if (len >= MAX_COLS) {
            fclose(f);
            return false;
        }

        // 复制行到地图中
        strcpy(labyrinth->map[labyrinth->rows], line);
        labyrinth->rows++;
    }

    fclose(f);

    // 检查是否至少读取了一行
    if (labyrinth->rows == 0) {
        return false;
    }

    return true;
}

// 在迷宫中查找指定的玩家
// 返回玩家的位置，如果未找到则返回 (-1, -1)
Position findPlayer(Labyrinth *labyrinth, char playerId) {
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            if (labyrinth->map[i][j] == playerId) {
                return (Position){i, j};
            }
        }
    }
    return (Position){-1, -1};
}

// 查找迷宫中第一个空地（按行优先顺序）
// 空地定义为 '.' 字符
// 如果找不到空地，返回 (-1, -1)
Position findFirstEmptySpace(Labyrinth *labyrinth) {
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            if (labyrinth->map[i][j] == '.') {
                return (Position){i, j};
            }
        }
    }
    return (Position){-1, -1};
}

// 检查指定位置是否为空地
// 空地定义为 '.' 字符
// 位置必须在迷宫范围内
bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    // 检查坐标是否在迷宫范围内
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return false;
    }
    return labyrinth->map[row][col] == '.';
}

// 移动玩家到指定方向
// 支持的方向：up, down, left, right
// 如果玩家不存在，则在第一个空地处生成玩家
// 返回 true 表示移动成功，false 表示移动失败（包括撞墙、方向非法等）
bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    // 验证方向参数
    int drow = 0, dcol = 0;
    if (strcmp(direction, "up") == 0) {
        drow = -1;
    } else if (strcmp(direction, "down") == 0) {
        drow = 1;
    } else if (strcmp(direction, "left") == 0) {
        dcol = -1;
    } else if (strcmp(direction, "right") == 0) {
        dcol = 1;
    } else {
        return false;
    }

    // 查找玩家当前位置
    Position pos = findPlayer(labyrinth, playerId);
    if (pos.row == -1) {
        // 玩家不存在，尝试在第一个空地处生成
        pos = findFirstEmptySpace(labyrinth);
        if (pos.row == -1) {
            // 没有空地，无法生成玩家
            return false;
        }
        // 在这个空地处放置玩家
        labyrinth->map[pos.row][pos.col] = playerId;
    }

    // 计算目标位置
    int new_row = pos.row + drow;
    int new_col = pos.col + dcol;

    // 检查目标位置是否有效且为空地
    if (!isEmptySpace(labyrinth, new_row, new_col)) {
        return false;
    }

    // 清除玩家原来的位置
    labyrinth->map[pos.row][pos.col] = '.';

    // 将玩家放置到新位置
    labyrinth->map[new_row][new_col] = playerId;

    return true;
}

// 保存迷宫地图到文件
// 返回 true 表示保存成功，false 表示保存失败
bool saveMap(Labyrinth *labyrinth, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        return false;
    }

    // 逐行写入地图
    for (int i = 0; i < labyrinth->rows; i++) {
        fprintf(f, "%s\n", labyrinth->map[i]);
    }

    fclose(f);
    return true;
}

// 深度优先搜索：从 (row, col) 开始，标记所有连通的空地
// visited 数组记录已访问的位置
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    // 检查坐标是否有效
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return;
    }

    // 检查是否已访问或者不是空地
    if (visited[row][col] || labyrinth->map[row][col] == '#') {
        return;
    }

    // 标记为已访问
    visited[row][col] = true;

    // 递归访问相邻的四个方向
    dfs(labyrinth, row - 1, col, visited);  // 上
    dfs(labyrinth, row + 1, col, visited);  // 下
    dfs(labyrinth, row, col - 1, visited);  // 左
    dfs(labyrinth, row, col + 1, visited);  // 右
}

// 检查迷宫的所有空地是否连通
// 如果所有空地都连通，返回 true；否则返回 false
bool isConnected(Labyrinth *labyrinth) {
    // 初始化 visited 数组
    bool visited[MAX_ROWS][MAX_COLS];
    memset(visited, false, sizeof(visited));

    // 查找第一个空地作为起点
    Position start = findFirstEmptySpace(labyrinth);
    if (start.row == -1) {
        // 没有空地，认为地图连通（空地集合为空）
        return true;
    }

    // 从第一个空地开始进行 DFS
    dfs(labyrinth, start.row, start.col, visited);

    // 检查所有空地是否都被访问过
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            // 如果找到未访问的空地，说明地图不连通
            if (labyrinth->map[i][j] != '#' && !visited[i][j]) {
                return false;
            }
        }
    }

    return true;
 }
