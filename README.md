# ThreadForge

一个 C++17 多线程池，带动态扩容 / 缩容、四级优先级 + 老化、任务取消、背压和异常隔离，
以及一个基于 Dear ImGui + Win32 + Direct3D 11 的实时仪表盘。

## 功能

- 动态扩容：积压达到阈值 → 增加工作线程（受上限与冷却时间约束）
- 动态缩容：长时间空闲 → 分步退役空闲线程（受下限约束）
- 四级优先级：Low / Normal / High / Critical，同优先级 FIFO
- 优先级老化：等待越久有效优先级越高，防止低优先级饿死
- 有界队列 + 背压：队列满时 `submit()` 返回空，调用方自行处理
- 任务取消：两段式。排队中由 `cancel()` 直接摘除；执行中置起取消标志，
  任务体通过 `CancellationToken::is_cancelled()` 自行退出（协作式，不强杀线程）
- 异常隔离：任务抛异常只标 `Failed`，不影响其他任务
- 优雅 / 非优雅关闭，可重复调用
- 运行时改配置，立即生效；非法值先夹紧再生效，`start()` 则按原始配置拒绝启动
- 指标快照 + 环形事件日志（任务级事件默认 1/32 采样，可开全量记录追单个任务）
- 桌面仪表盘：线程忙闲、积压、等待/吞吐曲线、等待队列、事件流、负载发生器
- 无界面 CLI：压测与演示
- 单元测试：22 个用例

## 技术栈

| 项 | 选择 |
| --- | --- |
| 语言 | C++17（内核只用标准库） |
| 编译器 | MinGW-64 g++ 8.1（posix 线程模型） |
| 构建 | CMake ≥ 3.20 + Ninja |
| 界面 | Dear ImGui 1.91.9b（`third_party/`，MIT） |
| 渲染 | Direct3D 11 |
| 测试 | 自带极简框架 + CTest |

## 目录结构

```
├─ CMakeLists.txt          构建入口
├─ CMakePresets.json       mingw-debug / mingw-release 预设
├─ include/threadpool/     线程池公开头文件
├─ src/                    线程池实现
├─ app/                    桌面界面（main / theme / dashboard）
├─ apps/cli_main.cpp       无界面压测程序
├─ tests/                  单元测试
├─ third_party/imgui/      内置 Dear ImGui
└─ build/                  构建产物（不进版本库）
```

## 构建与运行

```powershell
cmake --preset mingw-debug
cmake --build --preset build-debug
ctest --preset test-debug

.\build\debug\bin\threadforge_app.exe           # 打开仪表盘
.\build\debug\bin\threadforge_app.exe --demo    # 打开仪表盘并自动压测
.\build\debug\bin\threadforge_cli.exe --tasks 300 --threads 2 --max 8 --workload mixed
```

Release 版本：`cmake --preset mingw-release` + `cmake --build --preset build-release`。

## VSCode

打开本目录后：`Ctrl+Shift+B` 编译（Ninja），`F5` 选择调试 `threadforge_app` / `tp_tests` / `threadforge_cli`，
`Tasks: Run Task` 里有构建、测试、运行、清理等任务。编译器和头文件路径已在 `.vscode/settings.json` 配好。

## CLI 参数

```
--tasks N        提交任务总数（默认 300）
--threads N      初始/最少线程数（默认 2）
--max N          扩容上限（默认 8）
--batch N        每批提交数量（默认 25）
--pause MS       批间停顿毫秒（默认 20）
--workload KIND  cpu | io | mixed（默认 mixed）
--help           帮助
```

CLI 输出为 ASCII，避免控制台代码页（936/GBK）导致中文乱码。

## 第三方

界面基于 [Dear ImGui](https://github.com/ocornut/imgui)（MIT License，见 `third_party/imgui/LICENSE.txt`）。
