# MyProcessMonitor

Windows 10 22H2 x64 上运行的空闲期异常 CPU 监控工具。它在后台以 1 Hz 采样，在用户持续无输入时识别“总 CPU 不低、且持续一段时间”的进程组，记录事件并通过托盘提醒。

技术栈：

- C++17
- CMake ≥ 3.25 + Ninja
- MSVC 19.51（Visual Studio 2026 / v14.51 工具集）
- Win32 API（托盘、ListView，不依赖 Qt / WinUI / WebView2）
- SQLite（WAL）、spdlog、nlohmann/json、GoogleTest
- 依赖通过 vcpkg 管理，目标 triplet：`x64-windows-static`

架构、判定公式、数据库 schema、取舍与后续阶段见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## v0.1 已实现

- 常驻托盘应用 `MyProcessMonitor.exe`：
  - 单实例；
  - 托盘菜单：打开面板、暂停 30 分钟、恢复、打开数据目录、退出；
  - 托盘悬浮提示当前 CPU / 空闲秒数 / 暂停状态；
  - 异常事件气泡提醒；
  - 原生 Win32 面板：状态行 + 最近 40 条事件 ListView。
- 采样与归因核心：
  - `GetSystemTimes` + `NtQuerySystemInformation(SystemProcessInformation)`；
  - NT 路径失败时自动回退到 Toolhelp32 + `GetProcessTimes`；
  - 进程路径、发布者、Authenticode 签名、`svchost.exe` 服务映射；
  - 按镜像名聚合同组进程（例如多个 `chrome.exe`）；
  - 空闲判定使用 `GetLastInputInfo`、WTS 会话状态、`OpenInputDesktop`、全屏/演示检测；
  - 持续异常检测：触发阈值、持续时间、退出滞回、冷却、评分、已知维护任务上限；
  - 输入恢复、媒体/全屏抑制、前台进程降分。
- 事件与配置：
  - SQLite WAL 事件库；事件 JSON 可回读；
  - 90 天保留期，启动时清理；
  - 嵌套 JSON 配置，越界值修正并给出提示；
  - 5 MB × 3 滚动日志。
- CLI `mpm-cli.exe`：
  - `snapshot`：输出当前采样 JSON（Top 25 进程）；
  - `watch`：前台观察模式，打印事件与状态；
  - `events`：读取最近事件，支持 `--json`；
  - `doctor`：检查配置、采样器、空闲状态、数据库可写性。
- 测试：9 个 GoogleTest 用例，覆盖检测启动/结束、输入恢复、媒体抑制、启动宽限、同镜像聚合、配置加载/修正、SQLite upsert/prune。

## 构建

前置条件：

1. Visual Studio 2026（或可提供 MSVC 19.51 / v14.51 的 VS 安装），安装“使用 C++ 的桌面开发”。
2. CMake ≥ 3.25、Ninja。
3. vcpkg，并设置 `VCPKG_ROOT`，例如：

```powershell
$env:VCPKG_ROOT = "E:\Tools\vcpkg"
```

4. 首次配置时 vcpkg 会根据 `vcpkg.json` 安装依赖。当前 `vcpkg.json` 使用 baseline `a073c28e52d920cb2465a17512324b3b6684d03a`，triplet 为 `x64-windows-static`。

在 **Developer PowerShell for VS**（或先调用 `vcvars64.bat`）中：

```powershell
cmake --preset msvc-x64-debug
cmake --build --preset msvc-x64-debug
ctest --preset msvc-x64-debug --output-on-failure
```

Release：

```powershell
cmake --preset msvc-x64-release
cmake --build --preset msvc-x64-release
ctest --preset msvc-x64-release --output-on-failure
```

如果使用普通 PowerShell，先进入开发者环境：

```powershell
cmd /c "call ""E:\Tools\VisualStudio\VC\Auxiliary\Build\vcvars64.bat"" && set VCPKG_ROOT=E:\Tools\vcpkg && cmake --preset msvc-x64-debug"
```

产物：

- `build/msvc-x64-debug/MyProcessMonitor.exe`
- `build/msvc-x64-debug/mpm-cli.exe`
- `build/msvc-x64-debug/mpm_tests.exe`

## 运行

### 托盘应用

双击 `MyProcessMonitor.exe`。首次运行会创建：

```text
%LOCALAPPDATA%\MyProcessMonitor\
  config.json
  data\monitor.db
  logs\mpm.log
```

托盘图标双击打开面板；右键菜单可暂停/恢复、打开数据目录、退出。再次启动会唤出已有实例的面板。

### CLI

```powershell
# 当前采样快照（JSON）
.\mpm-cli.exe snapshot

# 观察 60 秒；--duration 0 表示一直运行，Ctrl+C 退出
.\mpm-cli.exe watch --duration 60

# 查看最近 20 条事件
.\mpm-cli.exe events --limit 20

# 导出最近事件为 JSON
.\mpm-cli.exe events --limit 50 --json

# 环境诊断
.\mpm-cli.exe doctor
```

`watch` / `events` / `doctor` 支持 `--data-dir <目录>` 指定独立数据目录；测试或排查时不会污染默认数据库。

## 配置

默认配置在 `%LOCALAPPDATA%\MyProcessMonitor\config.json`。完整示例见 [resources/config.example.json](resources/config.example.json)。

关键字段：

```json
{
  "sampling": { "intervalMs": 1000 },
  "idle": {
    "thresholdSeconds": 120,
    "treatLockedAsIdle": true,
    "suppressWhenMediaPlaying": true,
    "suppressWhenFullscreen": true
  },
  "detection": {
    "systemCpuTriggerPercent": 12.0,
    "minBusyCores": 0.75,
    "sustainSeconds": 30,
    "exitBelowPercent": 6.0,
    "exitSustainSeconds": 60,
    "cooldownSeconds": 900,
    "startupGraceSeconds": 180
  },
  "scoring": {
    "warningScore": 60,
    "criticalScore": 80,
    "knownMaintenanceCap": 65
  }
}
```

判定思路：

1. 用户空闲达到 `idle.thresholdSeconds`（或锁屏/息屏，取决于开关）；
2. 系统 CPU ≥ `detection.systemCpuTriggerPercent`；
3. 至少有一个可归因进程组，且 Top 组达到最小候选条件；
4. 条件连续满足 `detection.sustainSeconds`；
5. 进入事件后，低于 `exitBelowPercent` 持续 `exitSustainSeconds`，或用户恢复输入，事件结束；
6. 结束后 `cooldownSeconds` 内不重复触发；
7. 对 Microsoft/系统目录中的已知维护进程标记 `knownMaintenance`，评分上限 `knownMaintenanceCap`。

评分是 0–100 的启发式值，不是“恶意程度”。它综合系统 CPU 强度、持续时间、Top 组占比、未知程度与信任因素；`warningScore` / `criticalScore` 仅决定级别与是否弹气泡。

## 目录结构

```text
CMakeLists.txt
CMakePresets.json
vcpkg.json
include/mpm/
  core/       类型、配置、CPU 数学、检测器接口
  infra/      配置、路径、文本、日志、事件 JSON、SQLite
  win32/      NT/Toolhelp 采样、空闲状态、进程目录
  app/        MonitorSession
src/
  core/ infra/ win32/ app/ cli/
resources/
  app.manifest app.rc config.example.json
tests/
docs/ARCHITECTURE.md
```

## v0.1 限制

- 媒体播放检测尚未接入 Core Audio，`mediaPlaying` 当前恒为 `false`；全屏/演示检测已接入；
- 归因按镜像名聚合，尚未做进程树聚合；服务映射目前只解析 `svchost.exe` 的 `-s` 参数；同名但不同路径的进程会被合并到同一组；
- 尚未实现基线学习，评分中的“未知度”是固定值 0.35；
- 冷却期是全局的，不是按进程组；
- 未实现开机自启切换、允许列表、Markdown/CSV 导出、提权诊断模式；
- 不结束进程、不联网、不上传数据、不采集键盘/屏幕内容；
- 受保护进程（如部分系统进程）可能只能看到 `access_denied`，这是普通用户权限下的预期限制。

这些限制对应架构文档中的后续阶段，不影响 v0.1 的“后台采样 + 空闲异常识别 + 事件记录 + 托盘提醒”闭环。

## 测试

```powershell
ctest --preset msvc-x64-debug --output-on-failure
```

当前覆盖：

- 检测器：持续异常启动/结束、输入恢复、媒体抑制、启动宽限、同镜像聚合、已知维护上限；
- 配置：缺失文件、嵌套字段、越界修正、保存默认配置；
- 存储：SQLite upsert、按时间读取、保留期清理。

未包含真实 1 Hz 集成测试，避免在 CI/开发机上长期占用 CPU；采样与托盘路径已通过手工运行 `snapshot` / `watch` / `doctor` 验证。