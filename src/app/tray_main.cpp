#include "mpm/app/monitor_session.hpp"
#include "mpm/core/config.hpp"
#include "mpm/infra/config_loader.hpp"
#include "mpm/infra/logging.hpp"
#include "mpm/infra/paths.hpp"
#include "mpm/infra/sqlite_event_store.hpp"
#include "mpm/infra/text.hpp"

#include <spdlog/spdlog.h>

#include <Windows.h>
#include <CommCtrl.h>
#include <Shellapi.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace mpm;

namespace {

constexpr const wchar_t* kMainClass = L"MPM_MainWindow";
constexpr const wchar_t* kDashboardClass = L"MPM_DashboardWindow";
constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowDashboardMessage = WM_APP + 2;
constexpr UINT_PTR kTimerId = 1;

constexpr UINT kMenuOpen = 1001;
constexpr UINT kMenuPause = 1002;
constexpr UINT kMenuData = 1003;
constexpr UINT kMenuExit = 1004;
constexpr UINT kMenuRefresh = 1005;

class TrayApp;

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

class TrayApp {
public:
    explicit TrayApp(HINSTANCE instance) : instance_(instance) {}

    ~TrayApp() {
        if (session_) {
            session_->Stop();
        }
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
        }
    }

    int Run() {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&controls);

        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\MyProcessMonitor.Instance");
        if (mutex_ == nullptr) {
            MessageBoxW(nullptr, L"创建单实例互斥量失败。", L"MyProcessMonitor", MB_ICONERROR);
            return 1;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = nullptr;
            for (int attempt = 0; attempt < 20 && existing == nullptr; ++attempt) {
                existing = FindWindowW(kMainClass, L"MyProcessMonitor");
                if (existing == nullptr) {
                    Sleep(100);
                }
            }
            if (existing != nullptr) {
                PostMessageW(existing, kShowDashboardMessage, 0, 0);
            }
            return 0;
        }

        std::wstring directoryError;
        if (!EnsureAppDirectories(&directoryError)) {
            MessageBoxW(nullptr, directoryError.c_str(), L"MyProcessMonitor", MB_ICONERROR);
            return 1;
        }

        if (!std::filesystem::exists(ConfigFile())) {
            std::wstring saveError;
            if (!SaveDefaultConfig(ConfigFile(), &saveError)) {
                MessageBoxW(nullptr, saveError.c_str(), L"MyProcessMonitor", MB_ICONWARNING);
            }
        }

        std::wstring configWarning;
        config_ = LoadConfig(ConfigFile(), &configWarning);
        if (!configWarning.empty()) {
            OutputDebugStringW((configWarning + L"\n").c_str());
        }

        session_ = std::make_unique<MonitorSession>(config_, DatabaseFile(), LogDirectory());
        std::wstring startError;
        if (!session_->Start(&startError)) {
            MessageBoxW(nullptr, startError.c_str(), L"MyProcessMonitor", MB_ICONERROR);
            return 1;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance_;
        windowClass.lpfnWndProc = MainWindowProc;
        windowClass.lpszClassName = kMainClass;
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&windowClass);

        windowClass.lpfnWndProc = TrayApp::DashboardProc;
        windowClass.lpszClassName = kDashboardClass;
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&windowClass);
        Log()->info("窗口类注册完成");

        mainWindow_ = CreateWindowExW(0, kMainClass, L"MyProcessMonitor", WS_OVERLAPPED,
                                      0, 0, 0, 0, nullptr, nullptr, instance_, this);
        if (mainWindow_ == nullptr) {
            Log()->error("主窗口创建失败，GetLastError={}", GetLastError());
            MessageBoxW(nullptr, L"创建主窗口失败。", L"MyProcessMonitor", MB_ICONERROR);
            session_->Stop();
            return 1;
        }
        Log()->info("主窗口创建成功 hwnd={}", reinterpret_cast<std::uintptr_t>(mainWindow_));

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon();
        Log()->info("托盘图标添加结果={}", trayAdded_);
        SetTimer(mainWindow_, kTimerId, 1000, nullptr);
        UpdateUi();
        Log()->info("进入消息循环");

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        RemoveTrayIcon();
        if (session_) {
            session_->Stop();
        }
        ShutdownLogging();
        return 0;
    }

    void OnTrayMessage(LPARAM event) {
        switch (LOWORD(event)) {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowTrayMenu();
                break;
            case WM_LBUTTONDBLCLK:
                ShowDashboard();
                break;
            default:
                break;
        }
    }

    void OnTimer() {
        if (!trayAdded_) {
            AddTrayIcon();
        }
        UpdateUi();
    }

    void ShowDashboard() {
        if (dashboard_ == nullptr) {
            dashboard_ = CreateWindowExW(
                WS_EX_APPWINDOW, kDashboardClass, L"MyProcessMonitor 监控面板",
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 560, nullptr, nullptr,
                instance_, this);
            if (dashboard_ == nullptr) {
                return;
            }
        }
        ShowWindow(dashboard_, IsIconic(dashboard_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(dashboard_);
        FillDashboard(true);
    }

    void RefreshDashboard() {
        if (dashboard_ != nullptr) {
            FillDashboard(false);
        }
    }

    void OnDashboardDestroyed() { dashboard_ = nullptr; }

    void OnExit() {
        if (session_) {
            session_->Stop();
        }
        RemoveTrayIcon();
        if (dashboard_ != nullptr) {
            DestroyWindow(dashboard_);
            dashboard_ = nullptr;
        }
        if (mainWindow_ != nullptr) {
            DestroyWindow(mainWindow_);
            mainWindow_ = nullptr;
        }
    }

    void OnMainWindowClosing() { RemoveTrayIcon(); }

    HWND mainWindow() const { return mainWindow_; }
    HINSTANCE instance() const { return instance_; }
    MonitorSession* session() const { return session_.get(); }
    UINT taskbarCreatedMessage() const { return taskbarCreatedMessage_; }

    void HandleTaskbarCreated() {
        trayAdded_ = false;
        AddTrayIcon();
    }

private:
    void AddTrayIcon() {
        if (mainWindow_ == nullptr) {
            return;
        }
        nid_ = NOTIFYICONDATAW{};
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = mainWindow_;
        nid_.uID = kTrayId;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid_.uCallbackMessage = kTrayMessage;
        nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid_.szTip, L"MyProcessMonitor · 启动中");
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    }

    void RemoveTrayIcon() {
        if (trayAdded_) {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            trayAdded_ = false;
        }
    }

    void ShowTrayMenu() {
        if (mainWindow_ == nullptr) {
            return;
        }
        const MonitorStatus status = session_ != nullptr ? session_->Status() : MonitorStatus{};

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开监控面板");
        AppendMenuW(menu, MF_STRING, kMenuRefresh, L"刷新");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuPause,
                    status.paused ? L"恢复监控" : L"暂停监控 30 分钟");
        AppendMenuW(menu, MF_STRING, kMenuData, L"打开数据目录");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(mainWindow_);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, mainWindow_, nullptr);
        DestroyMenu(menu);
        PostMessageW(mainWindow_, WM_NULL, 0, 0);

        switch (command) {
            case kMenuOpen:
                ShowDashboard();
                break;
            case kMenuRefresh:
                FillDashboard(true);
                break;
            case kMenuPause:
                if (session_ != nullptr) {
                    if (status.paused) {
                        session_->Resume();
                    } else {
                        session_->PauseFor(std::chrono::minutes(30));
                    }
                    UpdateUi();
                }
                break;
            case kMenuData:
                ShellExecuteW(nullptr, L"open", AppDataDirectory().wstring().c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
                break;
            case kMenuExit:
                OnExit();
                break;
            default:
                break;
        }
    }

    void UpdateUi() {
        if (mainWindow_ == nullptr || session_ == nullptr) {
            return;
        }
        const MonitorStatus status = session_->Status();
        std::wstring tip = L"MyProcessMonitor";
        if (status.paused) {
            tip += L" · 已暂停";
        } else if (status.warmup) {
            tip += L" · 准备中";
        } else {
            wchar_t buffer[160]{};
            swprintf_s(buffer, L" · CPU %.1f%% · 空闲 %llus", status.systemCpu,
                       static_cast<unsigned long long>(status.idleSeconds));
            tip += buffer;
        }
        wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
        nid_.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid_);

        for (const auto& notification : session_->DrainNotifications()) {
            if (notification.notify) {
                ShowBalloon(notification.event);
            }
            RefreshDashboard();
        }
    }

    void ShowBalloon(const DetectionEvent& event) {
        NOTIFYICONDATAW info{};
        info.cbSize = sizeof(info);
        info.hWnd = mainWindow_;
        info.uID = kTrayId;
        info.uFlags = NIF_INFO;
        info.dwInfoFlags = NIIF_INFO;
        if (event.severity == Severity::Critical) {
            info.dwInfoFlags = NIIF_ERROR;
        } else if (event.severity == Severity::Warning) {
            info.dwInfoFlags = NIIF_WARNING;
        }
        info.uTimeout = 10000;
        wcscpy_s(info.szInfoTitle, L"检测到空闲期 CPU 异常");
        wchar_t body[256]{};
        swprintf_s(body, L"“%ls” 占用全系统 %.1f%%，评分 %d。", event.culprit.displayName.c_str(),
                   event.culprit.cpuPercent, event.score);
        wcsncpy_s(info.szInfo, body, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &info);
    }

    void FillDashboard(bool force) {
        if (dashboard_ == nullptr) {
            return;
        }
        HWND statusWindow = GetDlgItem(dashboard_, 2001);
        HWND list = GetDlgItem(dashboard_, 2002);
        if (statusWindow == nullptr || list == nullptr) {
            return;
        }

        const MonitorStatus status = session_ != nullptr ? session_->Status() : MonitorStatus{};
        wchar_t statusText[512]{};
        swprintf_s(statusText,
                   L"状态：%ls    系统 CPU：%.1f%%    空闲：%llus    Top：%ls %.1f%%    "
                   L"进程数：%u    事件：%llu",
                   status.paused ? L"已暂停" : (status.running ? L"监控中" : L"已停止"),
                   status.systemCpu, static_cast<unsigned long long>(status.idleSeconds),
                   status.topName.c_str(), status.topCpu, status.processCount,
                   static_cast<unsigned long long>(status.eventCount));
        SetWindowTextW(statusWindow, statusText);

        if (!force && list == nullptr) {
            return;
        }
        if (status.paused && !force) {
            return;
        }

        ListView_DeleteAllItems(list);
        if (session_ == nullptr || session_->Store() == nullptr) {
            return;
        }
        const auto events = session_->Store()->Recent(40);
        int row = 0;
        for (const auto& event : events) {
            const std::wstring time = Utf16(LocalTimeString(event.startedUnixMs));
            const std::wstring severity = Utf16(ToString(event.severity));
            const std::wstring name = event.culprit.displayName;
            wchar_t cpu[32]{};
            swprintf_s(cpu, L"%.1f%%", event.culprit.cpuPercent);
            const std::wstring score = std::to_wstring(event.score);
            std::wstring reason;
            for (const auto& item : event.reasons) {
                if (!reason.empty()) {
                    reason += L", ";
                }
                reason += Utf16(item);
                if (reason.size() > 90) {
                    reason += L"...";
                    break;
                }
            }

            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.iSubItem = 0;
            item.pszText = const_cast<wchar_t*>(time.c_str());
            const int index = ListView_InsertItem(list, &item);
            ListView_SetItemText(list, index, 1, const_cast<wchar_t*>(severity.c_str()));
            ListView_SetItemText(list, index, 2, const_cast<wchar_t*>(name.c_str()));
            ListView_SetItemText(list, index, 3, cpu);
            ListView_SetItemText(list, index, 4, const_cast<wchar_t*>(score.c_str()));
            ListView_SetItemText(list, index, 5, const_cast<wchar_t*>(reason.c_str()));
            ++row;
        }
    }

    static LRESULT CALLBACK DashboardProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_ = nullptr;
    HANDLE mutex_ = nullptr;
    HWND mainWindow_ = nullptr;
    HWND dashboard_ = nullptr;
    NOTIFYICONDATAW nid_{};
    bool trayAdded_ = false;
    UINT taskbarCreatedMessage_ = 0;
    AppConfig config_;
    std::unique_ptr<MonitorSession> session_;
};

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_CREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<TrayApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return 0;
    }

    if (app == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    if (app->taskbarCreatedMessage() != 0 && message == app->taskbarCreatedMessage()) {
        app->HandleTaskbarCreated();
        return 0;
    }

    switch (message) {
        case kTrayMessage:
            app->OnTrayMessage(lParam);
            return 0;
        case WM_TIMER:
            if (wParam == kTimerId) {
                app->OnTimer();
                return 0;
            }
            break;
        case kShowDashboardMessage:
            app->ShowDashboard();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kMenuOpen:
                    app->ShowDashboard();
                    return 0;
                case kMenuExit:
                    app->OnExit();
                    return 0;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            app->OnMainWindowClosing();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            app->OnMainWindowClosing();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK TrayApp::DashboardProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_CREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<TrayApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app == nullptr) {
            return -1;
        }

        HWND status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      12, 12, 850, 28, hwnd, reinterpret_cast<HMENU>(2001),
                                      app->instance(), nullptr);
        HWND list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 12, 48, 850,
            450, hwnd, reinterpret_cast<HMENU>(2002), app->instance(), nullptr);
        if (status != nullptr) {
            SendMessageW(status, WM_SETFONT,
                         reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        if (list != nullptr) {
            SendMessageW(list, WM_SETFONT,
                         reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            ListView_SetExtendedListViewStyle(
                list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            const wchar_t* headers[] = {L"时间", L"级别", L"进程/组", L"CPU", L"评分", L"原因"};
            const int widths[] = {160, 75, 210, 80, 60, 300};
            for (int index = 0; index < 6; ++index) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                column.pszText = const_cast<wchar_t*>(headers[index]);
                column.cx = widths[index];
                column.iSubItem = index;
                ListView_InsertColumn(list, index, &column);
            }
        }
        return 0;
    }

    if (app == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
        case WM_SIZE:
            if (app->session() != nullptr) {
                HWND status = GetDlgItem(hwnd, 2001);
                HWND list = GetDlgItem(hwnd, 2002);
                const int width = LOWORD(lParam);
                const int height = HIWORD(lParam);
                if (status != nullptr) {
                    MoveWindow(status, 12, 12, std::max(100, width - 24), 28, TRUE);
                }
                if (list != nullptr) {
                    MoveWindow(list, 12, 48, std::max(100, width - 24),
                               std::max(100, height - 60), TRUE);
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            app->OnDashboardDestroyed();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    TrayApp app(instance);
    return app.Run();
}