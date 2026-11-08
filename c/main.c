#include <initguid.h>
#include <windows.h>
#include <windowsx.h>

#include <commctrl.h>
#include <dbt.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <ntddvdeo.h>
#include <shellapi.h>
#include <stdlib.h>
#include <strsafe.h>

#include "resource.h"

#define MAX_MON 8
#define MAX_TARGETS 16
#define MON_NAME_CCH 128
#define MON_LABEL_CCH 160
#define MON_PATH_CCH 256
#define PCT_MAX 100
#define DEFAULT_VCP_MAX 100
#define VCP_BRIGHTNESS 0x10
#define TIMER_RESCAN 1
#define TIMER_HIDE 3
#define WM_TRAYICON (WM_USER + 1)
#define IDC_MONITOR 1
#define IDC_EXIT 2
#define WM_APP_SET_VCP (WM_APP + 10)
#define WM_APP_GET_VCP (WM_APP + 11)
#define WM_APP_REFRESH (WM_APP + 12)
#define WM_APP_SET_FAILED (WM_APP + 20)
#define WM_APP_GET_DONE (WM_APP + 21)
#define WM_APP_REFRESH_DONE (WM_APP + 22)
#define RESCAN_RETRY_BUDGET 10
#define RESCAN_INTERVAL_MS 1500
#define DDC_WRITE_RETRIES 3
#define DDC_WRITE_DELAY_MS 200
#define DDC_READ_RETRIES 3
#define DDC_READ_DELAY_MS 150

typedef struct {
    DWORD idx;
    DWORD pct;
    WCHAR path[MON_PATH_CCH];
} SET_REQ;

typedef struct {
    DWORD pct;
    BOOL ok;
    DWORD seq;
} GET_RES;

typedef struct {
    WCHAR label[MON_LABEL_CCH];
    WCHAR device_path[MON_PATH_CCH];
} MON_INFO;

typedef struct {
    PHYSICAL_MONITOR handle;
    DWORD vcp_max;
    MON_INFO info;
} MONITOR;

typedef struct {
    WCHAR path[MON_PATH_CCH];
    WCHAR name[MON_NAME_CCH];
} TARGET_NAME;

static HWND g_main, g_win, g_slider, g_label, g_combo;
static NOTIFYICONDATA g_nid;
static HDEVNOTIFY g_devnotify = NULL;
static UINT g_taskbar_msg = 0;
static MONITOR g_mons[MAX_MON];
static DWORD g_mon_cnt = 0;
static MON_INFO g_ui[MAX_MON];
static DWORD g_ui_cnt = 0;
static DWORD g_cur_mon = 0;
static WCHAR g_cur_devpath[MON_PATH_CCH] = {0};
static DWORD g_cur_pct = 50;
static BOOL g_cur_valid = FALSE;
static DWORD g_get_seq = 0;
static BOOL g_drag = FALSE;
static BOOL g_track = FALSE;
static DWORD g_retry_budget = 0;
static TARGET_NAME g_targets[MAX_TARGETS];
static DWORD g_target_cnt = 0;
static DWORD g_worker_tid = 0;
static HANDLE g_worker_thread = NULL;
static HANDLE g_worker_ready = NULL;
static CRITICAL_SECTION g_lock;

static void collect_targets(void)
{
    g_target_cnt = 0;
    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc) != ERROR_SUCCESS || !pc)
        return;

    DISPLAYCONFIG_PATH_INFO *paths = malloc(pc * sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = malloc((mc + 1) * sizeof(*modes));

    if (!paths || !modes) {
        free(paths);
        free(modes);
        return;
    }

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pc, paths, &mc, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < pc && g_target_cnt < MAX_TARGETS; i++) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {0};
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tn.header.size = sizeof(tn);
            tn.header.adapterId = paths[i].targetInfo.adapterId;
            tn.header.id = paths[i].targetInfo.id;

            if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS)
                continue;
            if (!tn.monitorFriendlyDeviceName[0] || !tn.monitorDevicePath[0])
                continue;

            (void)StringCchCopy(g_targets[g_target_cnt].path, MON_PATH_CCH, tn.monitorDevicePath);
            (void)StringCchCopy(g_targets[g_target_cnt].name, MON_NAME_CCH, tn.monitorFriendlyDeviceName);
            g_target_cnt++;
        }
    }
    free(paths);
    free(modes);
}

static BOOL get_monitor_device_path(HMONITOR hm, WCHAR *out, size_t cch)
{
    MONITORINFOEX mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfo(hm, (LPMONITORINFO)&mi))
        return FALSE;

    DISPLAY_DEVICE ad = {.cb = sizeof(ad)};
    if (!EnumDisplayDevices(mi.szDevice, 0, &ad, 0))
        return FALSE;

    DISPLAY_DEVICE md = {.cb = sizeof(md)};
    if (!EnumDisplayDevices(ad.DeviceName, 0, &md, EDD_GET_DEVICE_INTERFACE_NAME) || !md.DeviceID[0]) {
        ZeroMemory(&md, sizeof(md));
        md.cb = sizeof(md);
        if (!EnumDisplayDevices(mi.szDevice, 0, &md, EDD_GET_DEVICE_INTERFACE_NAME) || !md.DeviceID[0])
            return FALSE;
    }
    return SUCCEEDED(StringCchCopy(out, cch, md.DeviceID));
}

static BOOL resolve_monitor_name(HMONITOR hm, WCHAR *out, size_t cch)
{
    out[0] = 0;
    WCHAR path[MON_PATH_CCH];

    if (get_monitor_device_path(hm, path, _countof(path))) {
        for (DWORD i = 0; i < g_target_cnt; i++) {
            if (_wcsicmp(g_targets[i].path, path) == 0) {
                (void)StringCchCopy(out, cch, g_targets[i].name);
                return TRUE;
            }
        }
    }

    MONITORINFOEX mi = {.cbSize = sizeof(mi)};
    if (GetMonitorInfo(hm, (LPMONITORINFO)&mi)) {
        DISPLAY_DEVICE ad = {.cb = sizeof(ad)};
        if (EnumDisplayDevices(mi.szDevice, 0, &ad, 0)) {
            DISPLAY_DEVICE md = {.cb = sizeof(md)};
            if (EnumDisplayDevices(ad.DeviceName, 0, &md, 0) && md.DeviceString[0] &&
                _wcsicmp(md.DeviceString, L"Generic PnP Monitor") != 0 && _wcsicmp(md.DeviceString, L"Generic Monitor") != 0) {
                (void)StringCchCopy(out, cch, md.DeviceString);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL CALLBACK enum_proc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    (void)dc;
    (void)rc;
    (void)lp;
    DWORD cnt;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &cnt) || !cnt)
        return TRUE;

    WCHAR path[MON_PATH_CCH] = {0};
    get_monitor_device_path(hm, path, _countof(path));
    PHYSICAL_MONITOR *pm = malloc(cnt * sizeof(*pm));
    if (!pm)
        return TRUE;

    if (GetPhysicalMonitorsFromHMONITOR(hm, cnt, pm)) {
        DWORD i = 0;
        for (; i < cnt && g_mon_cnt < MAX_MON; i++) {
            DWORD cur, max;
            if (!GetVCPFeatureAndVCPFeatureReply(pm[i].hPhysicalMonitor, VCP_BRIGHTNESS, NULL, &cur, &max)) {
                DestroyPhysicalMonitor(pm[i].hPhysicalMonitor);
                continue;
            }

            MONITOR *m = &g_mons[g_mon_cnt];
            ZeroMemory(m, sizeof(*m));
            m->handle = pm[i];
            m->vcp_max = max ? max : DEFAULT_VCP_MAX;

            if (!resolve_monitor_name(hm, m->info.label, _countof(m->info.label)))
                (void)StringCchCopy(m->info.label, _countof(m->info.label), pm[i].szPhysicalMonitorDescription);
            if (!m->info.label[0])
                (void)StringCchCopy(m->info.label, _countof(m->info.label), L"Monitor");

            (void)StringCchCopy(m->info.device_path, _countof(m->info.device_path), path);
            g_mon_cnt++;
        }
        for (; i < cnt; i++)
            DestroyPhysicalMonitor(pm[i].hPhysicalMonitor);
    }
    free(pm);
    return TRUE;
}

static void label_disambiguate(void)
{
    for (DWORD i = 0; i < g_mon_cnt; i++) {
        for (DWORD j = 0; j < i; j++) {
            if (_wcsicmp(g_mons[i].info.label, g_mons[j].info.label) == 0) {
                WCHAR buf[MON_LABEL_CCH];
                (void)StringCchPrintf(buf, _countof(buf), L"%s (%u)", g_mons[i].info.label, (unsigned)(i + 1));
                (void)StringCchCopy(g_mons[i].info.label, _countof(g_mons[i].info.label), buf);
                break;
            }
        }
    }
}

static void mon_exit(void)
{
    for (DWORD i = 0; i < g_mon_cnt; i++) {
        if (g_mons[i].handle.hPhysicalMonitor)
            DestroyPhysicalMonitor(g_mons[i].handle.hPhysicalMonitor);
        ZeroMemory(&g_mons[i], sizeof(g_mons[i]));
    }
    g_mon_cnt = 0;
}

static void mon_refresh(void)
{
    mon_exit();
    collect_targets();
    EnumDisplayMonitors(NULL, NULL, enum_proc, 0);
    label_disambiguate();
}

static void publish_snapshot(void)
{
    EnterCriticalSection(&g_lock);
    for (DWORD i = 0; i < g_mon_cnt; i++)
        g_ui[i] = g_mons[i].info;
    g_ui_cnt = g_mon_cnt;
    LeaveCriticalSection(&g_lock);
}

static DWORD worker(LPVOID p)
{
    (void)p;
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    g_worker_tid = GetCurrentThreadId();
    SetEvent(g_worker_ready);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        switch (msg.message) {
        case WM_APP_SET_VCP: {
            SET_REQ *r = (SET_REQ *)msg.lParam;
            if (!r)
                break;

            BOOL ok = FALSE;

            if (r->idx < g_mon_cnt && _wcsicmp(g_mons[r->idx].info.device_path, r->path) == 0) {
                const DWORD max = g_mons[r->idx].vcp_max ? g_mons[r->idx].vcp_max : DEFAULT_VCP_MAX;
                const DWORD pct = r->pct > PCT_MAX ? PCT_MAX : r->pct;
                const DWORD target_value = pct * max / PCT_MAX;

                for (int attempt = 0; attempt < DDC_WRITE_RETRIES; ++attempt) {
                    ok = SetVCPFeature(g_mons[r->idx].handle.hPhysicalMonitor, VCP_BRIGHTNESS, target_value) != 0;
                    if (ok)
                        break;
                    Sleep(DDC_WRITE_DELAY_MS);
                }
            }

            if (!ok)
                PostMessage(g_main, WM_APP_SET_FAILED, r->idx, 0);
            free(r);
            break;
        }

        case WM_APP_GET_VCP: {
            const DWORD idx = (DWORD)msg.wParam, seq = (DWORD)msg.lParam;
            GET_RES *res = calloc(1, sizeof(*res));
            if (!res)
                break;

            res->seq = seq;
            if (idx < g_mon_cnt) {
                for (int attempt = 0; attempt < DDC_READ_RETRIES; ++attempt) {
                    DWORD cur, max;
                    if (GetVCPFeatureAndVCPFeatureReply(g_mons[idx].handle.hPhysicalMonitor, VCP_BRIGHTNESS, NULL, &cur, &max)) {
                        max = max ? max : DEFAULT_VCP_MAX;
                        g_mons[idx].vcp_max = max;
                        res->pct = cur >= max ? PCT_MAX : cur * PCT_MAX / max;
                        res->ok = TRUE;
                        break;
                    }
                    Sleep(DDC_READ_DELAY_MS);
                }
            }

            PostMessage(g_main, WM_APP_GET_DONE, 0, (LPARAM)res);
            break;
        }

        case WM_APP_REFRESH:
            mon_refresh();
            publish_snapshot();
            PostMessage(g_main, WM_APP_REFRESH_DONE, g_mon_cnt, 0);
            break;
        }
    }

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        if (msg.message == WM_APP_SET_VCP && msg.lParam)
            free((void *)msg.lParam);

    mon_exit();
    return 0;
}

static void request_get(const DWORD idx)
{
    if (!g_worker_tid)
        return;

    EnterCriticalSection(&g_lock);
    const BOOL valid = idx < g_ui_cnt;
    LeaveCriticalSection(&g_lock);
    g_cur_valid = FALSE;
    if (!valid)
        return;

    g_get_seq++;
    PostThreadMessage(g_worker_tid, WM_APP_GET_VCP, idx, g_get_seq);
}

static void request_set(const DWORD idx, const DWORD pct)
{
    if (!g_worker_tid)
        return;

    g_get_seq++;
    SET_REQ *r = calloc(1, sizeof(*r));
    if (!r)
        return;

    r->idx = idx;
    r->pct = pct;
    (void)StringCchCopy(r->path, _countof(r->path), g_cur_devpath);
    if (!PostThreadMessage(g_worker_tid, WM_APP_SET_VCP, 0, (LPARAM)r))
        free(r);
}

static void set_label(const DWORD pct)
{
    if (!IsWindow(g_label))
        return;

    WCHAR buf[32];
    (void)StringCchPrintf(buf, ARRAYSIZE(buf), L"%lu%%", pct);
    SetWindowText(g_label, buf);
}

static void sync_slider_label(void)
{
    if (!IsWindow(g_slider))
        return;

    EnterCriticalSection(&g_lock);
    const BOOL has = g_ui_cnt > 0;
    LeaveCriticalSection(&g_lock);

    if (has) {
        if (!g_drag)
            SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
        set_label(g_cur_pct);
    } else {
        SendMessage(g_slider, TBM_SETPOS, TRUE, 0);
        if (IsWindow(g_label))
            SetWindowText(g_label, L"No monitor");
    }
}

static void rebuild_combo(void)
{
    if (!IsWindow(g_combo))
        return;

    SendMessage(g_combo, CB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_lock);
    const DWORD cnt = g_ui_cnt;
    WCHAR labels[MAX_MON][MON_LABEL_CCH];

    for (DWORD i = 0; i < cnt; i++)
        (void)StringCchCopy(labels[i], MON_LABEL_CCH, g_ui[i].label);

    if (g_cur_mon >= cnt)
        g_cur_mon = 0;
    if (cnt)
        (void)StringCchCopy(g_cur_devpath, _countof(g_cur_devpath), g_ui[g_cur_mon].device_path);
    LeaveCriticalSection(&g_lock);

    for (DWORD i = 0; i < cnt; i++)
        SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)labels[i]);

    SendMessage(g_combo, CB_SETCURSEL, cnt ? g_cur_mon : (WPARAM)-1, 0);
}

static int slider_pos_from_x(HWND wnd, int x)
{
    RECT ch, th;
    SendMessage(wnd, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
    SendMessage(wnd, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
    const int thumb_w = th.right - th.left;
    const int usable = ch.right - ch.left - thumb_w;
    const int vmin = (int)SendMessage(wnd, TBM_GETRANGEMIN, 0, 0);
    const int vmax = (int)SendMessage(wnd, TBM_GETRANGEMAX, 0, 0);

    if (usable <= 0)
        return vmin;

    int cx = x - ch.left - thumb_w / 2;
    cx = cx < 0 ? 0 : cx > usable ? usable : cx;
    return vmin + (vmax - vmin) * cx / usable;
}

static LRESULT CALLBACK slider_sub(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)wp;
    (void)id;
    (void)ref;

    switch (msg) {
    case WM_LBUTTONDOWN: {
        const POINT pt = {.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)};
        RECT th;
        SendMessage(wnd, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        if (PtInRect(&th, pt)) {
            g_drag = TRUE;
            break;
        }

        const int pos = slider_pos_from_x(wnd, pt.x);
        SendMessage(wnd, TBM_SETPOS, TRUE, pos);
        SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)wnd);
        SetCapture(wnd);
        g_track = TRUE;
        g_drag = TRUE;
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_track) {
            const int pos = slider_pos_from_x(wnd, GET_X_LPARAM(lp));
            SendMessage(wnd, TBM_SETPOS, TRUE, pos);
            SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)wnd);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_track) {
            g_track = FALSE;
            g_drag = FALSE;
            ReleaseCapture();
            const int pos = (int)SendMessage(wnd, TBM_GETPOS, 0, 0);
            SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, pos), (LPARAM)wnd);
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        g_track = FALSE;
        g_drag = FALSE;
        break;

    default:;
    }

    return DefSubclassProc(wnd, msg, wp, lp);
}

static LRESULT CALLBACK slider_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_combo = CreateWindow(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 10, 10, 280, 200, wnd, (HMENU)IDC_MONITOR, NULL,
                               NULL);
        g_label = CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 50, 280, 20, wnd, NULL, NULL, NULL);
        g_slider = CreateWindow(TRACKBAR_CLASS, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ, 10, 80, 280, 30, wnd, NULL, NULL, NULL);
        SendMessage(g_slider, TBM_SETRANGE, TRUE, MAKELONG(0, PCT_MAX));
        SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
        SetWindowSubclass(g_slider, slider_sub, 1, 0);
        CreateWindow(L"BUTTON", L"Exit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, 115, 80, 25, wnd, (HMENU)IDC_EXIT, NULL, NULL);
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_MONITOR && HIWORD(wp) == CBN_SELCHANGE) {
            g_cur_mon = (DWORD)SendMessage(g_combo, CB_GETCURSEL, 0, 0);
            EnterCriticalSection(&g_lock);
            if (g_cur_mon < g_ui_cnt)
                (void)StringCchCopy(g_cur_devpath, _countof(g_cur_devpath), g_ui[g_cur_mon].device_path);
            LeaveCriticalSection(&g_lock);
            request_get(g_cur_mon);
        } else if (LOWORD(wp) == IDC_EXIT) {
            PostQuitMessage(0);
        }
        break;

    case WM_HSCROLL:
        if ((HWND)lp == g_slider) {
            const DWORD code = LOWORD(wp);
            if (code == TB_ENDTRACK || code == TB_THUMBPOSITION)
                g_drag = FALSE;
            const DWORD pos = (DWORD)SendMessage(g_slider, TBM_GETPOS, 0, 0);
            set_label(pos);

            if ((code == TB_ENDTRACK || code == TB_THUMBPOSITION || code == TB_PAGEUP || code == TB_PAGEDOWN || code == TB_LINEUP ||
                 code == TB_LINEDOWN) &&
                (pos != g_cur_pct || !g_cur_valid)) {
                request_set(g_cur_mon, pos);
                g_cur_pct = pos;
            }
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(wnd))
            SetTimer(wnd, TIMER_HIDE, 100, NULL);
        else
            KillTimer(wnd, TIMER_HIDE);
        break;

    case WM_TIMER:
        if (wp == TIMER_HIDE) {
            KillTimer(wnd, TIMER_HIDE);
            if (IsWindow(g_combo) && SendMessage(g_combo, CB_GETDROPPEDSTATE, 0, 0))
                return 0;
            if (GetForegroundWindow() != wnd)
                ShowWindow(wnd, SW_HIDE);
        }
        break;

    case WM_CLOSE:
        ShowWindow(wnd, SW_HIDE);
        break;

    default:
        return DefWindowProc(wnd, msg, wp, lp);
    }

    return 0;
}

static void show_window(void)
{

    g_track = FALSE;
    g_drag = FALSE;

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    RECT rc;

    if (hm && GetMonitorInfo(hm, &mi))
        rc = mi.rcWork;
    else
        SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);

    const int w = 300, h = 155;
    int x = pt.x - w / 2, y = pt.y - h - 20;

    if (x < rc.left)
        x = rc.left;
    if (x + w > rc.right)
        x = rc.right - w;
    if (y < rc.top)
        y = pt.y + 20;
    if (y + h > rc.bottom)
        y = rc.bottom - h;

    if (!IsWindow(g_win)) {
        g_win = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"br_slider", NULL, WS_POPUP | WS_BORDER, x, y, w, h, NULL, NULL,
                               GetModuleHandle(NULL), NULL);
        if (!g_win)
            return;
    } else {
        SetWindowPos(g_win, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    rebuild_combo();
    sync_slider_label();
    request_get(g_cur_mon);
    ShowWindow(g_win, SW_SHOW);
    SetForegroundWindow(g_win);
}

static void hide_window(void)
{
    if (IsWindow(g_win) && IsWindowVisible(g_win))
        ShowWindow(g_win, SW_HIDE);
}

static void schedule_rescan(const BOOL reset_budget)
{
    if (!g_main)
        return;
    if (reset_budget) {
        g_retry_budget = RESCAN_RETRY_BUDGET;
    }

    KillTimer(g_main, TIMER_RESCAN);
    SetTimer(g_main, TIMER_RESCAN, RESCAN_INTERVAL_MS, NULL);
}

static LRESULT CALLBACK main_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static ULONGLONG last_toggle = 0;

    if (g_taskbar_msg && msg == g_taskbar_msg) {

        Shell_NotifyIcon(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) {
            const ULONGLONG now = GetTickCount64();
            if (now - last_toggle < GetDoubleClickTime())
                return 0;

            last_toggle = now;

            if (IsWindow(g_win))
                KillTimer(g_win, TIMER_HIDE);
            if (IsWindow(g_win) && IsWindowVisible(g_win))
                hide_window();
            else
                show_window();
        }
        return 0;

    case WM_APP_SET_FAILED: {
        const DWORD idx = (DWORD)wp;
        if (idx == g_cur_mon) {
            g_cur_valid = FALSE;
            if (!g_drag)
                request_get(g_cur_mon);
        }
        return 0;
    }

    case WM_APP_GET_DONE: {
        GET_RES *r = (GET_RES *)lp;
        if (r) {
            if (r->seq == g_get_seq && r->ok && !g_drag) {
                g_cur_pct = r->pct;
                g_cur_valid = TRUE;
                sync_slider_label();
            }
            free(r);
        }
        return 0;
    }

    case WM_APP_REFRESH_DONE: {
        const DWORD cnt = (DWORD)wp;
        if (cnt > 0) {
            g_retry_budget = 0;
            DWORD new_idx = 0;
            BOOL matched_by_path = FALSE;
            EnterCriticalSection(&g_lock);

            if (g_cur_devpath[0]) {
                for (DWORD i = 0; i < g_ui_cnt; i++) {
                    if (_wcsicmp(g_ui[i].device_path, g_cur_devpath) == 0) {
                        new_idx = i;
                        matched_by_path = TRUE;
                        break;
                    }
                }
            }

            if (!matched_by_path && g_cur_mon < g_ui_cnt)
                new_idx = g_cur_mon;

            (void)StringCchCopy(g_cur_devpath, _countof(g_cur_devpath), g_ui[new_idx].device_path);
            LeaveCriticalSection(&g_lock);
            g_cur_mon = new_idx;
        } else {
            g_cur_mon = 0;
            g_cur_devpath[0] = 0;
        }

        g_cur_valid = FALSE;
        if (IsWindow(g_win) && IsWindowVisible(g_win)) {
            rebuild_combo();
            sync_slider_label();
        }

        if (cnt)
            request_get(g_cur_mon);
        else if (g_retry_budget > 0) {
            g_retry_budget--;
            schedule_rescan(FALSE);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_RESCAN) {
            KillTimer(wnd, TIMER_RESCAN);
            if (g_worker_tid)
                PostThreadMessage(g_worker_tid, WM_APP_REFRESH, 0, 0);
            return 0;
        }
        break;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            DEV_BROADCAST_HDR *h = (DEV_BROADCAST_HDR *)lp;
            if (h && h->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
                IsEqualGUID(&((DEV_BROADCAST_DEVICEINTERFACE *)h)->dbcc_classguid, &GUID_DEVINTERFACE_MONITOR)) {
                schedule_rescan(TRUE);
                return TRUE;
            }
        }
        break;

    case WM_DISPLAYCHANGE:
        schedule_rescan(TRUE);
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            schedule_rescan(TRUE);
            return TRUE;
        }
        break;

    default:
        break;
    }

    return DefWindowProc(wnd, msg, wp, lp);
}

int WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    (void)show;

    BOOL owned_icon = FALSE;
    HANDLE singleton = CreateMutex(NULL, TRUE, L"Local\\brightness");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton)
            CloseHandle(singleton);
        return 0;
    }

    InitializeCriticalSection(&g_lock);
    g_worker_ready = CreateEvent(NULL, TRUE, FALSE, NULL);

    const INITCOMMONCONTROLSEX icc = {.dwSize = sizeof(icc), .dwICC = ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEX wc = {.cbSize = sizeof(wc)};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"br_main";
    RegisterClassEx(&wc);

    wc.lpfnWndProc = slider_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"br_slider";
    RegisterClassEx(&wc);

    g_worker_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!g_worker_thread || WaitForSingleObject(g_worker_ready, 2000) != WAIT_OBJECT_0) {
        MessageBox(NULL, L"DDC worker failed to start.", L"Error", MB_ICONERROR);
        return 1;
    }

    g_main = CreateWindowEx(WS_EX_TOOLWINDOW, L"br_main", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_main) {
        PostThreadMessage(g_worker_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_worker_thread, 2000);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE f = {0};
    f.dbcc_size = sizeof(f);
    f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    f.dbcc_classguid = GUID_DEVINTERFACE_MONITOR;
    g_devnotify = RegisterDeviceNotification(g_main, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_taskbar_msg = RegisterWindowMessage(L"TaskbarCreated");

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (g_nid.hIcon)
        owned_icon = TRUE;
    else {
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        owned_icon = FALSE;
    }

    (void)StringCchCopy(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"brightness");
    Shell_NotifyIcon(NIM_ADD, &g_nid);

    g_retry_budget = RESCAN_RETRY_BUDGET;
    PostThreadMessage(g_worker_tid, WM_APP_REFRESH, 0, 0);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    BOOL worker_exited = TRUE;
    if (g_worker_thread) {
        PostThreadMessage(g_worker_tid, WM_QUIT, 0, 0);
        worker_exited = WaitForSingleObject(g_worker_thread, 5000) == WAIT_OBJECT_0;
        CloseHandle(g_worker_thread);
    }

    MSG drain;
    while (PeekMessage(&drain, g_main, WM_APP_GET_DONE, WM_APP_GET_DONE, PM_REMOVE))
        free((void *)drain.lParam);

    if (g_devnotify)
        UnregisterDeviceNotification(g_devnotify);
    if (IsWindow(g_win))
        DestroyWindow(g_win);
    if (IsWindow(g_main))
        DestroyWindow(g_main);

    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    if (owned_icon && g_nid.hIcon)
        DestroyIcon(g_nid.hIcon);

    if (worker_exited)
        DeleteCriticalSection(&g_lock);
    CloseHandle(g_worker_ready);
    return 0;
}
