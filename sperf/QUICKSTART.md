# sperf - 系统调用 Profiler 快速开始指南

## 编译

```bash
cd sperf
make
```

## 基本用法

### 版本信息
```bash
./sperf --version
# 输出: sperf version 1.0.0 - System Call Profiler
```

### 帮助信息
```bash
./sperf --help
```

### 分析系统调用

```bash
# 基本用法：分析ls命令的系统调用
strace -e trace=all ls -la 2>&1 | ./sperf

# 显示前10个系统调用
strace -e trace=all ls -la 2>&1 | ./sperf -n 10

# 按执行时间排序
strace -e trace=all ls -la 2>&1 | ./sperf -t

# 按系统调用名称排序
strace -e trace=all ls -la 2>&1 | ./sperf -s

# 组合选项：按时间排序显示前5个
strace -e trace=all python script.py 2>&1 | ./sperf -n 5 -t
```

## 输出格式

```
syscall                     calls    total(us)      avg(us)      max(us)      min(us)
-------------------- ------------ ------------ ------------ ------------ ------------
mmap                            3        54.00        18.00        25.00        11.00
write                           3        27.00         9.00        10.00         8.00
close                           2        15.00         7.50         8.00         7.00
-------------------- ------------ ------------ ------------ ------------ ------------
total                           8        96.00        12.00

Total unique system calls: 3
```

**字段说明**：
- `syscall` - 系统调用名称
- `calls` - 调用次数
- `total(us)` - 总执行时间（微秒）
- `avg(us)` - 平均执行时间（微秒）
- `max(us)` - 最大执行时间（微秒）
- `min(us)` - 最小执行时间（微秒）

## 命令行选项

| 选项 | 说明 | 例子 |
|------|------|------|
| `-n N` | 显示前N个系统调用（默认5个） | `./sperf -n 10` |
| `--top N` | `-n` 的长选项 | `./sperf --top 10` |
| `-t` | 按总执行时间排序 | `./sperf -t` |
| `--time` | `-t` 的长选项 | `./sperf --time` |
| `-s` | 按系统调用名称排序 | `./sperf -s` |
| `--sort` | `-s` 的长选项 | `./sperf --sort` |
| `-h` | 显示帮助信息 | `./sperf -h` |
| `--help` | `-h` 的长选项 | `./sperf --help` |
| `-V` | 显示版本信息 | `./sperf -V` |
| `--version` | `-V` 的长选项 | `./sperf --version` |

## 常见场景

### 找到最耗时的系统调用
```bash
strace -e trace=all <command> 2>&1 | ./sperf -t -n 10
```

### 找到调用最频繁的系统调用
```bash
strace -e trace=all <command> 2>&1 | ./sperf -n 20
```

### 查看所有系统调用（按名称）
```bash
strace -e trace=all <command> 2>&1 | ./sperf -s -n 100
```

### 分析Python脚本的性能
```bash
strace -e trace=all python my_script.py 2>&1 | ./sperf -t
```

### 分析特定程序的I/O行为
```bash
strace -e trace=open,read,write,close <command> 2>&1 | ./sperf
```

## 实现特性

- ✓ 准确的strace输出解析
- ✓ 完整的性能指标统计
- ✓ 多种排序和显示方式
- ✓ 详细的命令行选项
- ✓ 鲁棒的错误处理
- ✓ 高效的算法（O(n log n)）

## 技术细节

- **输入格式**：strace的详细跟踪输出（-e trace=all）
- **时间单位**：微秒（us）
- **最大系统调用数**：1024种不同的系统调用
- **性能**：处理10000行输入 < 500ms

## 进阶用法

### 保存分析结果到文件
```bash
strace -e trace=all <command> 2>&1 | ./sperf -t > analysis.txt
```

### 分析系统启动过程
```bash
strace -e trace=all /bin/bash -c "echo 'test'" 2>&1 | ./sperf -n 20
```

### 比较两个程序的系统调用行为
```bash
# 分析程序A
strace -e trace=all ./program_a 2>&1 | ./sperf -n 10 > a.txt

# 分析程序B
strace -e trace=all ./program_b 2>&1 | ./sperf -n 10 > b.txt

# 比较两个文件
diff a.txt b.txt
```

## 文档

- [DEVLOG.md](DEVLOG.md) - 详细的开发日志
- [REPORT.md](REPORT.md) - 完整的项目报告

