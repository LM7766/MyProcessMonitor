#include "mpm/win32/idle_provider.hpp"

#include <Windows.h>
#include <shellapi.h>
#include <wtsapi32.h>

namespace mpm {

ActivityState Win32IdleProvider::Query() {
    ActivityState state;

    LASTINPUTINFO lastInput{};
    lastInput.cbSize = sizeof(lastInput);
    if (GetLastInputInfo(&lastInput)) {
        const DWORD now = GetTickCount();
        const DWORD idleMs = now - lastInput.dwTime;
        state.idleSeconds = static_cast<std::uint64_t>(idleMs) / 1000ULL;
    }

    WTS_CONNECTSTATE_CLASS connectState = WTSDisconnected;
    bool stateKnown = false;
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSConnectState, &buffer, &bytes) &&
        buffer != nullptr && bytes >= sizeof(WTS_CONNECTSTATE_CLASS)) {
        connectState = *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(buffer);
        stateKnown = true;
    }
    if (buffer != nullptr) {
        WTSFreeMemory(buffer);
    }

    state.sessionActive = !stateKnown || connectState == WTSActive;
    if (stateKnown && !state.sessionActive) {
        state.locked = true;
    }

    // OpenInputDesktop fails on the secure desktop (lock screen / UAC), which
    // is exactly the state we want to classify as locked. It is a lightweight
    // poll and avoids a separate hidden window for session notifications.
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (desktop == nullptr) {
        state.locked = true;
    } else {
        CloseDesktop(desktop);
    }

    QUERY_USER_NOTIFICATION_STATE notificationState = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&notificationState))) {
        state.fullscreen =
            notificationState == QUNS_RUNNING_D3D_FULL_SCREEN ||
            notificationState == QUNS_PRESENTATION_MODE;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        state.foregroundPid = pid;
    }

    // v0.1 does not enumerate Core Audio sessions yet. The field is part of the
    // interface so a later provider can suppress false positives for video or
    // music playback without changing the detector.
    state.mediaPlaying = false;
    state.displayOff = false;
    return state;
}

} // namespace mpm