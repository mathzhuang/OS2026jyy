# M4 实验报告：C Read-Eval-Print-Loop (crepl)

## 一、实验目标

实现一个交互式的 C 语言编程环境，支持：

1. **函数定义与加载**：用户输入 C 函数定义，系统动态编译并将其加载到进程的符号空间
2. **表达式求值**：用户输入 C 表达式，系统计算其值并返回结果
3. **跨函数调用**：已加载的函数可被后续编译的函数或表达式调用
4. **错误处理**：正确检测和报告编译错误和运行时错误

## 二、核心设计

### 2.1 系统架构

```
┌─────────────────────────────────────────────┐
│  用户输入（函数定义 / 表达式）              │
└──────────────┬──────────────────────────────┘
               │
        ┌──────▼───────┐
        │ 包装 & 编写   │
        │  C 源代码文件 │
        └──────┬────────┘
               │
        ┌──────▼───────────────────────┐
        │  使用 GCC 编译为共享库 (.so)  │
        │  gcc -fPIC -shared ...        │
        └──────┬──────────────────────┘
               │
        ┌──────▼──────────────────────┐
        │ 使用 dlopen() 动态加载库     │
        │ 符号对后续代码可见（GLOBAL）  │
        └──────┬────────────────────┐
               │                    │
        ┌──────▼──────┐     ┌──────▼──────┐
        │ 函数定义    │     │ 表达式求值  │
        │ 保存库指针  │     │ 查找并执行  │
        └─────────────┘     └─────────────┘
```

### 2.2 关键数据结构

```c
// 全局库列表 - 维持已加载库的生命周期
static void* loaded_libs[MAX_LIBS];
static int lib_count = 0;

// 临时文件计数器 - 确保文件名唯一性
static int file_counter = 0;
```

### 2.3 主要函数设计

#### compile_to_shared_lib()
- **功能**：编译 C 源文件为位置无关的共享库
- **编译命令**：
  ```bash
  gcc -fPIC -shared -o output.so input.c 2>/dev/null
  ```
- **标志说明**：
  - `-fPIC`：生成位置无关代码
  - `-shared`：生成共享库
  - `2>/dev/null`：隐藏编译警告

#### compile_and_load_function()
- **输入**：C 函数定义字符串
- **处理流程**：
  1. 生成唯一的临时文件名
  2. 写入函数定义
  3. 编译为 .so 文件
  4. 使用 `RTLD_GLOBAL | RTLD_LAZY` 加载
  5. 保存库指针
  6. 清理源文件
- **返回值**：编译加载成功返回 true，否则 false
- **关键设计**：
  - `RTLD_GLOBAL`：使该库中的函数对后续代码可见
  - `RTLD_LAZY`：延迟绑定允许前向引用

#### evaluate_expression()
- **输入**：C 表达式字符串、结果指针
- **处理流程**：
  1. 生成包装函数：
     ```c
     int __crepl_eval_func() { return <expression>; }
     ```
  2. 编译为 .so 文件
  3. 使用 `RTLD_NOW` 加载（立即符号绑定）
  4. 使用 `dlsym()` 查找函数符号
  5. 调用函数获取返回值
  6. 卸载库并清理文件
- **返回值**：求值成功返回 true，否则 false
- **关键设计**：
  - `RTLD_NOW`：立即符号绑定，未定义函数在 dlopen 时就会失败
  - 这确保调用未定义函数返回 false 而非运行时错误

## 三、实现细节

### 3.1 函数间调用机制

通过 `RTLD_GLOBAL` 标志实现函数间的符号可见性：

```
第1步：编译函数 A                    第2步：编译函数 B（调用 A）
┌──────────────────┐                ┌──────────────────┐
│ int test() {     │                │ int test2() {    │
│   return 42;     │                │   return test(); │
│ }                │                │ }                │
└────────┬─────────┘                └────────┬─────────┘
         │                                   │
    gcc编译                             gcc编译
         │                                   │
    生成test.so                       生成test2.so
         │                                   │
  dlopen(GLOBAL)                    dlopen(NOW)
         │                                   │
   符号表中添加test          ◄──────查找test符号──────┘
         │
   保存指针不卸载
```

### 3.2 表达式求值流程

```c
// 用户输入: "21 + 21"
// 生成的代码:
int __crepl_eval_func() { return 21 + 21; }

// 该函数编译后:
// - 独立编译为 .so
// - RTLD_NOW 加载确保所有引用的符号都已定义
// - dlsym() 查找 __crepl_eval_func
// - 直接调用获得返回值 42
// - dlclose() 卸载库（一次性使用）
```

### 3.3 错误处理策略

| 错误类型 | 检测点 | 处理方式 |
|---------|------|--------|
| 编译错误 | system() 返回值 | 返回 false |
| 加载失败 | dlopen() 返回值 | 返回 false |
| 符号未定义 | dlopen(RTLD_NOW) | 返回 false |
| 符号查找失败 | dlsym() 返回值 | 返回 false |
| 文件操作失败 | fopen/fwrite 返回值 | 返回 false |

## 四、测试结果

### 4.1 测试用例覆盖

| 测试名称 | 测试内容 | 结果 |
|---------|--------|------|
| test_compile_valid_function | 编译有效的函数定义 | ✅ PASS |
| test_compile_function_using_previous | 新编译函数调用旧编译函数 | ✅ PASS |
| test_compile_invalid_syntax | 检测编译时语法错误 | ✅ PASS |
| test_evaluate_simple_constant | 求值常数表达式 | ✅ PASS |
| test_evaluate_arithmetic | 求值算术表达式 | ✅ PASS |
| test_evaluate_function_call | 求值函数调用表达式 | ✅ PASS |
| test_evaluate_complex_expression | 求值复杂表达式 | ✅ PASS |
| test_evaluate_undefined_function | 检测调用未定义函数 | ✅ PASS |
| test_evaluate_syntax_error | 检测表达式语法错误 | ✅ PASS |

**最终结果**：9/9 测试通过 ✅

### 4.2 测试输出

```
TestKit
- [PASS] test_compile_valid_function (./tests.c:11)
- [PASS] test_compile_function_using_previous (./tests.c:16)
- [PASS] test_compile_invalid_syntax (./tests.c:23)
- [PASS] test_evaluate_simple_constant (./tests.c:28)
- [PASS] test_evaluate_arithmetic (./tests.c:35)
- [PASS] test_evaluate_function_call (./tests.c:42)
- [PASS] test_evaluate_complex_expression (./tests.c:51)
- [PASS] test_evaluate_undefined_function (./tests.c:60)
- [PASS] test_evaluate_syntax_error (./tests.c:66)
- 9/9 test cases passed.
```

## 五、关键技术决策

### 5.1 为什么使用动态编译？

- **优点**：
  1. 用户可动态提供代码，灵活性高
  2. 充分利用 GCC 优化，代码性能好
  3. 支持完整的 C 语言特性
  
- **权衡**：
  1. 编译开销（毫秒级，可接受）
  2. 需要 GCC 和动态链接库支持

### 5.2 为什么使用共享库而不是 JIT？

- **共享库方案优势**：
  1. 利用现有基础设施（GCC、glibc）
  2. 代码简单可靠
  3. 充分利用操作系统机制

- **JIT 方案劣势**：
  1. 实现复杂度高
  2. 需要机器码生成

### 5.3 RTLD_NOW vs RTLD_LAZY 选择

| 场景 | 选择 | 原因 |
|------|------|------|
| 加载编译的函数 | RTLD_LAZY | 允许前向引用，新函数可调用后编译的函数 |
| 求值表达式 | RTLD_NOW | 立即检测未定义符号，快速失败 |

---

## 六、性能分析

### 6.1 时间复杂度

```
编译函数：O(n)，其中 n 是代码长度（编译时间占主导）
求值表达式：O(m)，其中 m 是表达式长度

编译通常在 1-10ms（取决于代码大小和系统负载）
```

### 6.2 空间复杂度

```
加载的库：O(k)，k 是编译的函数个数
临时文件：O(k)，但会被清理
```

## 七、可能的扩展

### 7.1 短期改进

1. **REPL 交互界面**
   - 添加 readline 支持
   - 交互式输入提示

2. **符号表管理**
   - 列举已加载函数
   - 支持函数卸载

3. **更好的错误诊断**
   - 捕获编译器错误信息
   - 提供行号和错误描述

### 7.2 中期改进

1. **表达式缓存**
   - 频繁使用的表达式预编译
   - 提高性能

2. **更多类型支持**
   - 浮点数表达式
   - 字符串处理
   - 结构体

3. **安全沙箱**
   - 限制资源使用
   - 防止恶意代码

## 八、代码质量

### 8.1 注释覆盖率

- 所有公开函数：详细的功能和参数说明
- 关键算法步骤：逐行注释
- 设计决策：说明原因和权衡

### 8.2 错误处理

- 每个系统调用都有返回值检查
- 清理所有临时资源（即使出错）
- 一致的错误返回策略（false 表示失败）

### 8.3 代码风格

- 统一的命名规范
- 清晰的函数职责划分
- 合理的代码组织

## 九、总结

本实验成功实现了一个功能完整的 C REPL，核心特性包括：

1. ✅ 动态编译函数定义
2. ✅ 动态加载函数到符号空间
3. ✅ 表达式求值
4. ✅ 函数间调用
5. ✅ 完善的错误处理
6. ✅ 所有测试通过

通过这个实验，深入理解了：

- 动态编译和链接的机制
- dlopen/dlsym/dlclose 的使用
- 符号可见性和全局符号表的管理
- 运行时代码加载的原理和应用

代码简洁、注释详细、容易理解和维护，为后续功能扩展奠定了坚实基础。

