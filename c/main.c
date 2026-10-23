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
#define TIMER_RESCAN 1
#define TIMER_HIDE_DELAY 3
#define WM_APP_SET_VCP (WM_APP + 10)
#define WM_APP_GET_VCP (WM_APP + 11)
#define WM_APP_REFRESH (WM_APP + 12)
#define WM_APP_SET_FAILED (WM_APP + 13)
#define WM_APP_GET_DONE (WM_APP + 20)
#define WM_APP_REFRESH_DONE (WM_APP + 21)

typedef struct {
    DWORD idx;
    DWORD pct;
} SET_REQ;

typedef struct {
    DWORD idx;
    DWORD pct;
    BOOL ok;
    DWORD seq;
} GET_RES;

typedef struct {
    PHYSICAL_MONITOR handle;
    DWORD vcp_max;
    WCHAR label[MON_LABEL_CCH];
    WCHAR device_path[MON_PATH_CCH];
} MONITOR;

typedef struct {
    WCHAR path[MON_PATH_CCH];
    WCHAR name[MON_NAME_CCH];
} TARGET_NAME;

static HWND g_main, g_win, g_slider, g_label, g_combo;
static NOTIFYICONDATA g_nid;
static BOOL g_nid_owned_icon = FALSE;
static HDEVNOTIFY g_devnotify = NULL;
static UINT g_taskbar_msg = 0;
static ULONGLONG g_last_toggle = 0;
static MONITOR g_mons[MAX_MON];
static DWORD g_mon_cnt = 0;
static DWORD g_cur_mon = 0;
static WCHAR g_cur_devpath[MON_PATH_CCH] = {0};
static DWORD g_cur_pct = 50;
static BOOL g_cur_valid = FALSE;
static DWORD g_get_seq = 0;
static BOOL g_drag = FALSE;
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
    DISPLAYCONFIG_MODE_INFO *modes = malloc(mc * sizeof(*modes));

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

            (void)StringCchCopyW(g_targets[g_target_cnt].path, MON_PATH_CCH, tn.monitorDevicePath);
            (void)StringCchCopyW(g_targets[g_target_cnt].name, MON_NAME_CCH, tn.monitorFriendlyDeviceName);
            g_target_cnt++;
        }
    }
    free(paths);
    free(modes);
}

static BOOL get_monitor_device_path(HMONITOR hm, WCHAR *out, size_t cch)
{
    MONITORINFOEXW mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfoW(hm, (LPMONITORINFO)&mi))
        return FALSE;

    DISPLAY_DEVICEW ad = {.cb = sizeof(ad)};
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &ad, 0))
        return FALSE;

    DISPLAY_DEVICEW md = {.cb = sizeof(md)};
    if (!EnumDisplayDevicesW(ad.DeviceName, 0, &md, EDD_GET_DEVICE_INTERFACE_NAME) || !md.DeviceID[0]) {
        ZeroMemory(&md, sizeof(md));
        md.cb = sizeof(md);
        if (!EnumDisplayDevicesW(mi.szDevice, 0, &md, EDD_GET_DEVICE_INTERFACE_NAME) || !md.DeviceID[0])
            return FALSE;
    }
    return SUCCEEDED(StringCchCopyW(out, cch, md.DeviceID));
}

static BOOL resolve_monitor_name(HMONITOR hm, WCHAR *out, size_t cch)
{
    out[0] = 0;
    WCHAR path[MON_PATH_CCH];

    if (get_monitor_device_path(hm, path, _countof(path))) {
        for (int i = 0; i < g_target_cnt; i++) {
            if (_wcsicmp(g_targets[i].path, path) == 0) {
                (void)StringCchCopyW(out, cch, g_targets[i].name);
                return TRUE;
            }
        }
    }

    MONITORINFOEXW mi = {.cbSize = sizeof(mi)};
    if (GetMonitorInfoW(hm, (LPMONITORINFO)&mi)) {
        DISPLAY_DEVICEW ad = {.cb = sizeof(ad)};
        if (EnumDisplayDevicesW(mi.szDevice, 0, &ad, 0)) {
            DISPLAY_DEVICEW md = {.cb = sizeof(md)};
            if (EnumDisplayDevicesW(ad.DeviceName, 0, &md, 0) && md.DeviceString[0] &&
                _wcsicmp(md.DeviceString, L"Generic PnP Monitor") != 0 && _wcsicmp(md.DeviceString, L"Generic Monitor") != 0) {
                (void)StringCchCopyW(out, cch, md.DeviceString);
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
            if (!GetVCPFeatureAndVCPFeatureReply(pm[i].hPhysicalMonitor, 0x10, NULL, &cur, &max)) {
                DestroyPhysicalMonitor(pm[i].hPhysicalMonitor);
                continue;
            }

            MONITOR *m = &g_mons[g_mon_cnt];
            ZeroMemory(m, sizeof(*m));
            m->handle = pm[i];
            m->vcp_max = max ? max : 100;

            if (!resolve_monitor_name(hm, m->label, _countof(m->label)))
                (void)StringCchCopyW(m->label, _countof(m->label), pm[i].szPhysicalMonitorDescription);
            if (!m->label[0])
                (void)StringCchCopyW(m->label, _countof(m->label), L"Monitor");

            (void)StringCchCopyW(m->device_path, _countof(m->device_path), path);
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
            if (_wcsicmp(g_mons[i].label, g_mons[j].label) == 0) {
                WCHAR buf[MON_LABEL_CCH];
                (void)StringCchPrintfW(buf, _countof(buf), L"%s (%u)", g_mons[i].label, (unsigned)(i + 1));
                (void)StringCchCopyW(g_mons[i].label, _countof(g_mons[i].label), buf);
                break;
            }
        }
    }
}

static void mon_exit_locked(void)
{
    for (DWORD i = 0; i < g_mon_cnt; i++) {
        if (g_mons[i].handle.hPhysicalMonitor)
            DestroyPhysicalMonitor(g_mons[i].handle.hPhysicalMonitor);

        ZeroMemory(&g_mons[i], sizeof(g_mons[i]));
    }
    g_mon_cnt = 0;
}

static void mon_init_locked(void)
{
    g_mon_cnt = 0;
    collect_targets();
    EnumDisplayMonitors(NULL, NULL, enum_proc, 0);
    label_disambiguate();
}

static DWORD worker(LPVOID p)
{
    (void)p;
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    g_worker_tid = GetCurrentThreadId();
    SetEvent(g_worker_ready);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_APP_SET_VCP) {
            SET_REQ *r = (SET_REQ *)msg.lParam;
            if (!r)
                continue;

            BOOL ok = FALSE;
            EnterCriticalSection(&g_lock);

            if (r->idx < g_mon_cnt) {
                const DWORD max = g_mons[r->idx].vcp_max ? g_mons[r->idx].vcp_max : 100;
                const DWORD pct = r->pct > 100 ? 100 : r->pct;
                ok = SetVCPFeature(g_mons[r->idx].handle.hPhysicalMonitor, 0x10, pct * max / 100) != 0;
            }

            LeaveCriticalSection(&g_lock);
            if (!ok)
                PostMessage(g_main, WM_APP_SET_FAILED, r->idx, 0);
            free(r);
        } else if (msg.message == WM_APP_GET_VCP) {
            const DWORD idx = (DWORD)msg.wParam, seq = (DWORD)msg.lParam;
            GET_RES *res = calloc(1, sizeof(*res));
            if (!res)
                continue;

            res->idx = idx;
            res->seq = seq;
            EnterCriticalSection(&g_lock);

            if (idx < g_mon_cnt) {
                DWORD cur, max;
                if (GetVCPFeatureAndVCPFeatureReply(g_mons[idx].handle.hPhysicalMonitor, 0x10, NULL, &cur, &max)) {
                    max = max ? max : 100;
                    g_mons[idx].vcp_max = max;
                    res->pct = cur >= max ? 100 : cur * 100 / max;
                    res->ok = TRUE;
                }
            }

            LeaveCriticalSection(&g_lock);
            PostMessage(g_main, WM_APP_GET_DONE, 0, (LPARAM)res);
        } else if (msg.message == WM_APP_REFRESH) {
            EnterCriticalSection(&g_lock);
            mon_exit_locked();
            mon_init_locked();
            const DWORD cnt = g_mon_cnt;
            LeaveCriticalSection(&g_lock);
            PostMessage(g_main, WM_APP_REFRESH_DONE, cnt, 0);
        }
    }

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        if (msg.message == WM_APP_SET_VCP && msg.lParam)
            free((void *)msg.lParam);

    EnterCriticalSection(&g_lock);
    mon_exit_locked();
    LeaveCriticalSection(&g_lock);
    return 0;
}

static void request_get(const DWORD idx)
{
    if (!g_worker_tid)
        return;

    EnterCriticalSection(&g_lock);
    const BOOL valid = idx < g_mon_cnt;
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
    SET_REQ *r = malloc(sizeof(*r));
    if (!r)
        return;

    r->idx = idx;
    r->pct = pct;
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
    const BOOL has = g_mon_cnt > 0;
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
    const DWORD cnt = g_mon_cnt;
    WCHAR labels[MAX_MON][MON_LABEL_CCH];

    for (DWORD i = 0; i < cnt; i++)
        (void)StringCchCopyW(labels[i], MON_LABEL_CCH, g_mons[i].label);

    LeaveCriticalSection(&g_lock);
    if (g_cur_mon >= cnt)
        g_cur_mon = 0;

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
            g_drag = FALSE;
            break;
        }

        const int pos = slider_pos_from_x(wnd, pt.x);
        SendMessage(wnd, TBM_SETPOS, TRUE, pos);
        SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)wnd);
        SetCapture(wnd);
        g_drag = TRUE;
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_drag) {
            const int pos = slider_pos_from_x(wnd, GET_X_LPARAM(lp));
            SendMessage(wnd, TBM_SETPOS, TRUE, pos);
            SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)wnd);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_drag) {
            g_drag = FALSE;
            ReleaseCapture();
            const int pos = (int)SendMessage(wnd, TBM_GETPOS, 0, 0);
            SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, pos), (LPARAM)wnd);
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
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
        g_combo = CreateWindow(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 10, 10, 280, 200, wnd, (HMENU)1, NULL, NULL);
        g_label = CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 50, 280, 20, wnd, NULL, NULL, NULL);
        g_slider = CreateWindow(TRACKBAR_CLASS, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ, 10, 80, 280, 30, wnd, NULL, NULL, NULL);
        SendMessage(g_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
        SetWindowSubclass(g_slider, slider_sub, 1, 0);
        CreateWindow(L"BUTTON", L"Exit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, 115, 80, 25, wnd, (HMENU)2, NULL, NULL);
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == 1 && HIWORD(wp) == CBN_SELCHANGE) {
            g_cur_mon = (DWORD)SendMessage(g_combo, CB_GETCURSEL, 0, 0);
            EnterCriticalSection(&g_lock);
            if (g_cur_mon < g_mon_cnt)
                (void)StringCchCopyW(g_cur_devpath, _countof(g_cur_devpath), g_mons[g_cur_mon].device_path);

            LeaveCriticalSection(&g_lock);
            request_get(g_cur_mon);
        } else if (LOWORD(wp) == 2) {
            PostQuitMessage(0);
        }
        break;

    case WM_HSCROLL:
        if ((HWND)lp == g_slider) {
            const DWORD code = LOWORD(wp);
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
            SetTimer(wnd, TIMER_HIDE_DELAY, 100, NULL);
        else
            KillTimer(wnd, TIMER_HIDE_DELAY);
        break;

    case WM_TIMER:
        if (wp == TIMER_HIDE_DELAY) {
            KillTimer(wnd, TIMER_HIDE_DELAY);
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
    if (reset_budget)
        g_retry_budget = 2;

    KillTimer(g_main, TIMER_RESCAN);
    SetTimer(g_main, TIMER_RESCAN, 600, NULL);
}

static LRESULT CALLBACK main_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_taskbar_msg && msg == g_taskbar_msg) {
        Shell_NotifyIcon(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
    case WM_USER + 1:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) {
            const ULONGLONG now = GetTickCount64();
            if (now - g_last_toggle < GetDoubleClickTime())
                return 0;

            g_last_toggle = now;

            if (IsWindow(g_win))
                KillTimer(g_win, TIMER_HIDE_DELAY);
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
            EnterCriticalSection(&g_lock);

            if (g_cur_devpath[0]) {
                for (DWORD i = 0; i < g_mon_cnt; i++) {
                    if (_wcsicmp(g_mons[i].device_path, g_cur_devpath) == 0) {
                        new_idx = i;
                        break;
                    }
                }
            } else if (g_cur_mon < g_mon_cnt) {
                new_idx = g_cur_mon;
            }

            (void)StringCchCopyW(g_cur_devpath, _countof(g_cur_devpath), g_mons[new_idx].device_path);
            LeaveCriticalSection(&g_lock);
            g_cur_mon = new_idx;
            g_cur_valid = FALSE;

            if (IsWindow(g_win) && IsWindowVisible(g_win)) {
                rebuild_combo();
                sync_slider_label();
            }

            request_get(g_cur_mon);
        } else {
            g_cur_mon = 0;
            g_cur_devpath[0] = 0;
            g_cur_valid = FALSE;

            if (IsWindow(g_win) && IsWindowVisible(g_win)) {
                rebuild_combo();
                sync_slider_label();
            }
            if (g_retry_budget > 0) {
                g_retry_budget--;
                schedule_rescan(FALSE);
            }
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
                IsEqualGUID(&((DEV_BROADCAST_DEVICEINTERFACE_W *)h)->dbcc_classguid, &GUID_DEVINTERFACE_MONITOR)) {
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
        MessageBoxW(NULL, L"DDC worker failed to start.", L"Error", MB_ICONERROR);
        return 1;
    }

    g_main = CreateWindowEx(WS_EX_TOOLWINDOW, L"br_main", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_main) {
        PostThreadMessage(g_worker_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_worker_thread, 2000);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W f = {0};
    f.dbcc_size = sizeof(f);
    f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    f.dbcc_classguid = GUID_DEVINTERFACE_MONITOR;
    g_devnotify = RegisterDeviceNotificationW(g_main, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_taskbar_msg = RegisterWindowMessage(L"TaskbarCreated");

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_USER + 1;
    g_nid.hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (g_nid.hIcon)
        g_nid_owned_icon = TRUE;
    else {
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        g_nid_owned_icon = FALSE;
    }

    (void)StringCchCopy(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"brightness");
    Shell_NotifyIcon(NIM_ADD, &g_nid);

    g_retry_budget = 2;
    PostThreadMessage(g_worker_tid, WM_APP_REFRESH, 0, 0);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    if (g_worker_thread) {
        PostThreadMessage(g_worker_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_worker_thread, INFINITE);
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
    if (g_nid_owned_icon && g_nid.hIcon)
        DestroyIcon(g_nid.hIcon);

    DeleteCriticalSection(&g_lock);
    CloseHandle(g_worker_ready);
    return 0;
}
