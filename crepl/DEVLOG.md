# M4 crepl 实验开发日志

## 实验概述
本实验实现一个 C Read-Eval-Print-Loop (REPL) 环境，允许用户动态编译和执行 C 代码。该实现需要支持：
1. 动态编译函数定义并将其加载到进程中
2. 编译和执行用户输入的表达式
3. 已加载函数之间的相互调用
4. 错误处理和验证

---

## 开发过程详细记录

### 阶段 1：需求分析与设计 (14:00-14:15)

**任务内容**
- 查看项目文档 (https://jyywiki.cn/OS/2026/labs/M4.md)
- 分析测试用例 (tests.c)
- 理解核心功能需求

**关键发现**
- 实验涉及 9 个测试用例，覆盖函数编译、表达式求值、错误处理三大方面
- 测试用例包括：
  - 有效函数编译
  - 函数间调用（先编译的函数可被后编译的函数调用）
  - 语法错误检测
  - 表达式求值（常量、算术、函数调用）
  - 未定义函数错误处理
  - 表达式语法错误处理

**初步设计方案**
- 使用 GCC 动态编译 C 代码为共享库 (.so)
- 使用 dlopen/dlsym 进行动态加载和符号查找
- 维护已加载库列表，确保函数间可见性
- 为表达式创建包装函数以便编译执行

---

### 阶段 2：核心功能实现 (14:15-14:45)

#### 2.1 辅助函数实现

**compile_to_shared_lib()**
- 功能：使用 GCC 编译 C 源文件为共享库
- 实现方式：
  ```c
  gcc -fPIC -shared -o output.so input.c
  ```
- 通过 system() 调用编译器
- 检查返回值判断编译是否成功

**write_source_file()**
- 功能：将 C 代码字符串写入临时文件
- 实现方式：使用 fopen/fwrite/fclose
- 返回写入的字节数是否等于字符串长度

#### 2.2 compile_and_load_function() 实现

**核心流程**
1. 生成唯一的临时文件名 `/tmp/crepl_N.c` 和 `/tmp/crepl_N.so`
2. 将函数定义写入 C 文件
3. 使用 GCC 编译为共享库
4. 使用 dlopen() 加载库，flags 为 `RTLD_GLOBAL | RTLD_LAZY`
   - RTLD_GLOBAL：使库中符号对后续加载的库可见
   - RTLD_LAZY：延迟符号绑定
5. 保存库指针到 loaded_libs[] 数组
6. 清理源文件（.so 保留，因为仍被加载）

**关键设计决策**
- 使用 RTLD_GLOBAL 确保库间符号可见
- 维护全局库列表，避免被 GC
- 每个编译的代码生成独立的 .so 文件

#### 2.3 evaluate_expression() 实现

**核心流程**
1. 生成唯一的表达式函数文件 `/tmp/crepl_expr_N.c`
2. 生成包装函数代码：
   ```c
   int __crepl_eval_func() { return <expression>; }
   ```
3. 编译为共享库
4. 使用 dlopen() 加载库
5. 使用 dlsym() 查找 `__crepl_eval_func` 符号
6. 调用函数获取返回值
7. 卸载库并清理临时文件

**初期遇到的问题**
- 表达式求值后如何处理未定义函数的调用
- dlopen/dlsym 的错误处理方式

---

### 阶段 3：测试与调试 (14:45-15:30)

#### 3.1 首次编译运行

**问题 1：测试框架不输出结果**
- 原因：需要设置 TK_RUN=1 环境变量
- 解决方案：`TK_RUN=1 ./crepl`

**问题 2：test_evaluate_undefined_function 测试失败**
- 现象：
  ```
  ./crepl: symbol lookup error: /tmp/crepl_expr_0.so: undefined symbol: undefined_function
  - [FAIL] test_evaluate_undefined_function (./tests.c:60) - FAIL (127)
  ```
- 根本原因：当表达式调用未定义函数时，dlopen 返回成功（RTLD_LAZY），但在运行时才发现符号不存在

#### 3.2 问题诊断与解决

**尝试方案 1：使用 --unresolved-symbols=ignore-in-object-files**
- 目标：允许编译时保留未定义符号
- 结果：无法通过编译验证，问题依然存在

**尝试方案 2：使用 -Wl,-z,defs**
- 目标：强制检查所有符号定义
- 结果：表达式编译失败（因为调用的函数在当前编译单元中未定义），影响了 test_evaluate_function_call

**最终方案：使用 RTLD_NOW 替代 RTLD_LAZY**
- 实现方式：
  ```c
  void* handle = dlopen(so_file, RTLD_NOW);  // 立即符号绑定
  ```
- 工作原理：
  - RTLD_NOW 在 dlopen 时立即解析所有符号
  - 若符号未定义，dlopen 返回 NULL
  - 这样可以在加载时（而非执行时）检测到未定义函数
- 结果：✅ 所有 9 个测试通过

**关键理解**
- RTLD_LAZY vs RTLD_NOW 的区别在于符号绑定时机
- 对于表达式求值，需要立即绑定确保调用未定义函数时快速失败
- 对于函数定义，使用 RTLD_LAZY 允许前向引用，但需要 RTLD_GLOBAL 才能让后续库找到符号

---

### 阶段 4：代码优化与清理 (15:30-15:40)

#### 4.1 移除未使用代码

**删除 generate_function_declarations()**
- 该函数原本计划维护函数声明列表
- 实现中发现不需要（通过 dlopen/dlsym 机制自动处理）
- 移除后减少编译警告

#### 4.2 代码注释完善

添加详细中文注释说明：
- 各函数的功能和参数含义
- 关键算法步骤
- 设计决策的原因
- 编译和链接标志的含义

---

## 最终测试结果

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
- 9/9 test cases passed. ✅
```

---

## 技术细节总结

### 使用的关键系统调用和库

| 功能 | 实现方式 | 关键参数 |
|------|--------|--------|
| 代码编译 | system() + gcc | `-fPIC -shared` |
| 库加载 | dlopen() | `RTLD_NOW`, `RTLD_GLOBAL`, `RTLD_LAZY` |
| 符号查找 | dlsym() | 函数名字符串 |
| 临时文件 | snprintf() + fopen/fwrite | `/tmp/crepl_N.{c,so}` |
| 文件删除 | unlink() | 清理临时源文件 |

### 关键设计模式

1. **动态编译包装**
   - 将用户输入包装成完整的 C 函数
   - 编译为独立的共享库
   - 动态加载和执行

2. **符号可见性管理**
   - RTLD_GLOBAL 使编译函数对后续代码可见
   - 通过库加载顺序维护依赖关系

3. **错误检测分层**
   - 编译时：使用 system() 返回值
   - 加载时：使用 dlopen() 返回值
   - 符号查找：使用 dlsym() 返回值

---

## 性能考虑

- 每次求值都编译新代码，有一定开销
- 临时文件管理：.c 文件立即删除，.so 程序退出时被 OS 清理
- 库保持加载状态直到程序退出

---

## 可能的改进方向

1. **符号表维护**：维护编译的函数列表，支持 REPL 交互时的函数列举

2. **性能优化**：对频繁执行的表达式进行缓存

3. **错误信息**：收集编译器错误输出，返回给用户进行调试

4. **类型支持**：扩展支持更多基本类型的表达式求值

5. **内存管理**：实现更细粒度的库卸载策略

---

## 开发经验总结

1. **调试技巧**
   - 使用 TK_VERBOSE 环境变量查看失败测试的输出
   - 临时文件可用于检查生成的代码内容

2. **关键突破**
   - 理解 dlopen flags 的含义（尤其是 RTLD_NOW vs RTLD_LAZY）
   - 认识到动态链接错误的两个阶段（加载时 vs 执行时）

3. **测试驱动开发**
   - 逐个通过测试用例，快速发现设计缺陷
   - 最后失败的测试用例反映了需要特殊处理的边界情况

