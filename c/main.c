#include <windows.h>
#include <initguid.h>
#include <windowsx.h>
#include <commctrl.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <dbt.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>
#include <setupapi.h>
#include <ntddvdeo.h>
#include "resource.h"

#define MAX_TARGETS   16
#define MAX_MON       8
#define MON_NAME_CCH  128
#define MON_LABEL_CCH 160
#define MON_PATH_CCH  256

#define TIMER_RESCAN       1
#define TIMER_RESCAN_RETRY 2
#define TIMER_HIDE_DELAY   3

#define WM_APP_REQ_REFRESH  (WM_APP + 10)
#define WM_APP_REQ_SET_VCP  (WM_APP + 11)
#define WM_APP_REQ_GET_VCP  (WM_APP + 12)
#define WM_APP_SET_FAILED   (WM_APP + 13)
#define WM_APP_DONE_REFRESH (WM_APP + 20)
#define WM_APP_DONE_GET_VCP (WM_APP + 21)

typedef struct {
    DWORD mon_index;
    DWORD pct;
} VCP_REQUEST;

typedef struct {
    DWORD mon_index;
    DWORD pct;
    BOOL ok;
    DWORD seq;
} VCP_RESULT;

typedef struct {
    WCHAR device_path[MON_PATH_CCH];
    WCHAR friendly[MON_NAME_CCH];
} TARGET_NAME_ENTRY;

typedef struct {
    PHYSICAL_MONITOR handle;
    DWORD vcp_max;
    WCHAR friendly_name[MON_NAME_CCH];
    WCHAR display_label[MON_LABEL_CCH];
    WCHAR device_path[MON_PATH_CCH];
} MonitorEntry;

static TARGET_NAME_ENTRY g_target_names[MAX_TARGETS];
static int g_target_name_cnt = 0;
static HWND g_main, g_win, g_slider, g_label, g_combo;
static NOTIFYICONDATA g_nid;
static BOOL g_nid_icon_owned = FALSE;
static HDEVNOTIFY g_mon_notify = NULL;
static UINT g_taskbar_created_msg = 0;
static ULONGLONG g_last_toggle_tick = 0;
static ULONGLONG g_last_devchange_tick = 0;
static MonitorEntry g_monitors[MAX_MON];
static DWORD g_mon_cnt = 0;
static DWORD g_cur_mon = 0;
static WCHAR g_cur_device_path[MON_PATH_CCH] = {0};
static DWORD g_cur_pct = 50;
static BOOL g_cur_pct_valid = FALSE;
static DWORD g_get_seq = 0;
static BOOL g_slider_drag = FALSE;
static int g_retry_budget = 0;
static DWORD g_worker_tid = 0;
static HANDLE g_worker_thread = NULL;
static HANDLE g_worker_ready_event = NULL;
static CRITICAL_SECTION g_mon_lock;

static BOOL mon_get_pct(const DWORD i, DWORD *pct) {
    DWORD cur, max;
    if (i >= g_mon_cnt) return FALSE;
    if (!GetVCPFeatureAndVCPFeatureReply(g_monitors[i].handle.hPhysicalMonitor, 0x10, NULL, &cur, &max)) return FALSE;
    max = max ? max : 100;
    *pct = (cur >= max) ? 100 : (cur * 100 / max);
    g_monitors[i].vcp_max = max;
    return TRUE;
}

static BOOL mon_set_pct(const DWORD i, DWORD pct) {
    if (i >= g_mon_cnt) return FALSE;
    if (pct > 100) pct = 100;
    const DWORD max = g_monitors[i].vcp_max ? g_monitors[i].vcp_max : 100;
    const DWORD vcp = pct * max / 100;
    return SetVCPFeature(g_monitors[i].handle.hPhysicalMonitor, 0x10, vcp) != 0;
}

static BOOL parse_edid_name(const BYTE *edid, const DWORD len, WCHAR *out, const size_t out_cch) {
    if (!edid || len < 128) return FALSE;
    for (int i = 0; i < 4; i++) {
        const BYTE *p = edid + 54 + i * 18;
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0xFC) {
            char name_a[14] = {0};
            memcpy(name_a, p + 5, 13);
            for (int k = 12; k >= 0 && (name_a[k] == ' ' || name_a[k] == '\n' || name_a[k] == '\r'); k--) name_a[k] = '\0';
            if (name_a[0] && MultiByteToWideChar(CP_ACP, 0, name_a, -1, out, (int) out_cch) > 0) return TRUE;
        }
    }
    return FALSE;
}

static BOOL get_monitor_interface_id(HMONITOR hm, WCHAR *out, size_t out_cch) {
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (LPMONITORINFO) &mi)) return FALSE;

    DISPLAY_DEVICEW adapter;
    ZeroMemory(&adapter, sizeof(adapter));
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &adapter, 0)) return FALSE;

    DISPLAY_DEVICEW mon;
    ZeroMemory(&mon, sizeof(mon));
    mon.cb = sizeof(mon);
    if (!EnumDisplayDevicesW(adapter.DeviceName, 0, &mon, EDD_GET_DEVICE_INTERFACE_NAME) || !mon.DeviceID[0]) {
        ZeroMemory(&mon, sizeof(mon));
        mon.cb = sizeof(mon);
        if (!EnumDisplayDevicesW(mi.szDevice, 0, &mon, EDD_GET_DEVICE_INTERFACE_NAME) || !mon.DeviceID[0]) return FALSE;
    }
    return SUCCEEDED(StringCchCopyW(out, out_cch, mon.DeviceID));
}

static void collect_target_friendly_names(void) {
    g_target_name_cnt = 0;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return;
    if (path_count == 0) return;

    DISPLAYCONFIG_PATH_INFO *paths = malloc(path_count * sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = malloc(mode_count * sizeof(*modes));
    if (!paths || !modes) {
        free(paths);
        free(modes);
        return;
    }

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < path_count && g_target_name_cnt < MAX_TARGETS; i++) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
            ZeroMemory(&tn, sizeof(tn));
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tn.header.size = sizeof(tn);
            tn.header.adapterId = paths[i].targetInfo.adapterId;
            tn.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
            if (!tn.monitorFriendlyDeviceName[0] || !tn.monitorDevicePath[0]) continue;

            TARGET_NAME_ENTRY *e = &g_target_names[g_target_name_cnt];
            if (FAILED(StringCchCopyW(e->friendly, _countof(e->friendly), tn.monitorFriendlyDeviceName))) continue;
            if (FAILED(StringCchCopyW(e->device_path, _countof(e->device_path), tn.monitorDevicePath))) continue;
            g_target_name_cnt++;
        }
    }

    free(paths);
    free(modes);
}

static BOOL get_target_friendly_name(HMONITOR hm, WCHAR *out, size_t out_cch) {
    if (g_target_name_cnt == 0) return FALSE;
    WCHAR device_id[MON_PATH_CCH];
    if (!get_monitor_interface_id(hm, device_id, _countof(device_id))) return FALSE;

    for (int i = 0; i < g_target_name_cnt; i++) {
        if (_wcsicmp(g_target_names[i].device_path, device_id) == 0) {
            if (SUCCEEDED(StringCchCopyW(out, out_cch, g_target_names[i].friendly))) return out[0] != L'\0';
        }
    }
    return FALSE;
}

static BOOL get_friendly_monitor_name(HMONITOR hm, WCHAR *out_name, size_t out_cch) {
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (LPMONITORINFO) &mi)) return FALSE;

    DISPLAY_DEVICEW adapter;
    ZeroMemory(&adapter, sizeof(adapter));
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &adapter, 0)) return FALSE;

    DISPLAY_DEVICEW mon;
    ZeroMemory(&mon, sizeof(mon));
    mon.cb = sizeof(mon);
    if (!EnumDisplayDevicesW(adapter.DeviceName, 0, &mon, 0)) return FALSE;
    if (mon.DeviceString[0] == L'\0' || _wcsicmp(mon.DeviceString, L"Generic PnP Monitor") == 0 || _wcsicmp(
            mon.DeviceString, L"Generic Monitor") == 0)
        return FALSE;
    if (FAILED(StringCchCopyW(out_name, out_cch, mon.DeviceString))) return FALSE;
    return TRUE;
}

static BOOL get_edid_name_from_device_key(const WCHAR *device_key, WCHAR *out, const size_t out_cch) {
    if (!device_key || !device_key[0]) return FALSE;
    const WCHAR *reg_path = device_key;
    if (wcsncmp(reg_path, L"\\Registry\\Machine\\", 18) == 0) reg_path += 18;

    WCHAR full_path[512];
    if (FAILED(StringCchPrintfW(full_path, _countof(full_path), L"%s\\Device Parameters", reg_path))) return FALSE;

    HKEY hkey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full_path, 0, KEY_READ, &hkey) != ERROR_SUCCESS) return FALSE;

    BYTE edid[512];
    DWORD edid_len = sizeof(edid);
    DWORD type = 0;
    BOOL ok = FALSE;
    if (RegQueryValueExW(hkey, L"EDID", NULL, &type, edid, &edid_len) == ERROR_SUCCESS) ok = parse_edid_name(edid, edid_len, out, out_cch);
    RegCloseKey(hkey);
    return ok;
}

static BOOL get_edid_name_for_physical_monitor(HMONITOR hm, WCHAR *out, size_t out_cch) {
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (LPMONITORINFO) &mi)) return FALSE;

    DISPLAY_DEVICEW adapter;
    ZeroMemory(&adapter, sizeof(adapter));
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &adapter, 0)) return FALSE;

    DISPLAY_DEVICEW mon;
    ZeroMemory(&mon, sizeof(mon));
    mon.cb = sizeof(mon);
    if (!EnumDisplayDevicesW(adapter.DeviceName, 0, &mon, EDD_GET_DEVICE_INTERFACE_NAME)) return FALSE;
    if (mon.DeviceKey[0] && get_edid_name_from_device_key(mon.DeviceKey, out, out_cch)) return TRUE;
    if (!mon.DeviceID[0]) return FALSE;

    const HDEVINFO dev_info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_MONITOR, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return FALSE;

    SP_DEVICE_INTERFACE_DATA if_data;
    ZeroMemory(&if_data, sizeof(if_data));
    if_data.cbSize = sizeof(if_data);
    BOOL found = FALSE;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL, &GUID_DEVINTERFACE_MONITOR, i, &if_data); i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, NULL, 0, &size, NULL);
        if (size == 0) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = malloc(size);
        if (!detail) continue;
        ZeroMemory(detail, size);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA dev_data;
        ZeroMemory(&dev_data, sizeof(dev_data));
        dev_data.cbSize = sizeof(dev_data);
        BOOL matched = FALSE;
        if (SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, detail, size, NULL, &dev_data))
            matched = _wcsicmp(detail->DevicePath, mon.DeviceID) == 0;

        if (matched) {
            HKEY hkey = SetupDiOpenDevRegKey(dev_info, &dev_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (hkey) {
                BYTE edid[512];
                DWORD edid_len = sizeof(edid);
                DWORD type = 0;
                if (RegQueryValueExW(hkey, L"EDID", NULL, &type, edid, &edid_len) == ERROR_SUCCESS)
                    found = parse_edid_name(edid, edid_len, out, out_cch);
                RegCloseKey(hkey);
            }
        }

        free(detail);
        if (found) break;
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return found;
}

static BOOL resolve_monitor_name(HMONITOR hm, WCHAR *out, size_t out_cch) {
    out[0] = L'\0';
    return get_target_friendly_name(hm, out, out_cch) || get_friendly_monitor_name(hm, out, out_cch) ||
           get_edid_name_for_physical_monitor(hm, out, out_cch);
}

static BOOL CALLBACK enum_proc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp) {
    (void) dc;
    (void) rc;
    (void) lp;

    DWORD cnt;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &cnt) || !cnt) return TRUE;

    WCHAR device_path[MON_PATH_CCH] = {0};
    (void) get_monitor_interface_id(hm, device_path, _countof(device_path));

    PHYSICAL_MONITOR *mons = malloc(cnt * sizeof(PHYSICAL_MONITOR));
    if (!mons) return TRUE;

    if (GetPhysicalMonitorsFromHMONITOR(hm, cnt, mons)) {
        DWORD i = 0;
        for (; i < cnt && g_mon_cnt < MAX_MON; i++) {
            DWORD cur, max;
            if (!GetVCPFeatureAndVCPFeatureReply(mons[i].hPhysicalMonitor, 0x10, NULL, &cur, &max)) {
                DestroyPhysicalMonitor(mons[i].hPhysicalMonitor);
                continue;
            }

            (void) cur;
            MonitorEntry *e = &g_monitors[g_mon_cnt];
            ZeroMemory(e, sizeof(*e));
            e->handle = mons[i];
            e->vcp_max = max ? max : 100;
            (void) resolve_monitor_name(hm, e->friendly_name, _countof(e->friendly_name));
            (void) StringCchCopyW(e->device_path, _countof(e->device_path), device_path);
            g_mon_cnt++;
        }

        for (; i < cnt; i++) DestroyPhysicalMonitor(mons[i].hPhysicalMonitor);
    }

    free(mons);
    return TRUE;
}

static void disambiguate_display_labels(void) {
    for (DWORD i = 0; i < g_mon_cnt; i++) {
        const WCHAR *base = g_monitors[i].friendly_name[0] ? g_monitors[i].friendly_name : L"Monitor";
        BOOL dup = FALSE;

        for (DWORD j = 0; j < g_mon_cnt && !dup; j++) {
            if (i == j) continue;
            const WCHAR *other = g_monitors[j].friendly_name[0] ? g_monitors[j].friendly_name : L"Monitor";
            if (_wcsicmp(base, other) == 0) dup = TRUE;
        }

        if (dup)
            (void) StringCchPrintfW(g_monitors[i].display_label, _countof(g_monitors[i].display_label), L"%s (%u)", base,
                                    (unsigned) (i + 1));
        else (void) StringCchCopyW(g_monitors[i].display_label, _countof(g_monitors[i].display_label), base);
    }
}

static void mon_init_locked(void) {
    g_mon_cnt = 0;
    collect_target_friendly_names();
    EnumDisplayMonitors(NULL, NULL, enum_proc, 0);
    disambiguate_display_labels();
}

static void mon_exit_locked(void) {
    for (DWORD i = 0; i < g_mon_cnt; i++) {
        if (g_monitors[i].handle.hPhysicalMonitor) DestroyPhysicalMonitor(g_monitors[i].handle.hPhysicalMonitor);
        ZeroMemory(&g_monitors[i], sizeof(g_monitors[i]));
    }
    g_mon_cnt = 0;
}

static DWORD WINAPI vcp_worker(LPVOID param) {
    (void) param;
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    g_worker_tid = GetCurrentThreadId();
    SetEvent(g_worker_ready_event);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_APP_REQ_SET_VCP) {
            VCP_REQUEST *req = (VCP_REQUEST *) msg.lParam;
            if (req) {
                MSG peek;
                while (PeekMessage(&peek, NULL, WM_APP_REQ_SET_VCP, WM_APP_REQ_SET_VCP, PM_REMOVE)) {
                    free(req);
                    req = (VCP_REQUEST *) peek.lParam;
                }

                const DWORD idx = req->mon_index;
                const DWORD pct = req->pct;
                EnterCriticalSection(&g_mon_lock);
                const BOOL ok = mon_set_pct(idx, pct);
                LeaveCriticalSection(&g_mon_lock);
                free(req);
                if (!ok)
                    PostMessage(g_main, WM_APP_SET_FAILED, idx, 0);
            }
        } else if (msg.message == WM_APP_REQ_GET_VCP) {
            const DWORD idx = (DWORD) msg.wParam;
            const DWORD seq = (DWORD) msg.lParam;
            VCP_RESULT *res = calloc(1, sizeof(VCP_RESULT));
            if (res) {
                res->mon_index = idx;
                res->seq = seq;
                EnterCriticalSection(&g_mon_lock);
                res->ok = idx < g_mon_cnt && mon_get_pct(idx, &res->pct);
                LeaveCriticalSection(&g_mon_lock);
                PostMessage(g_main, WM_APP_DONE_GET_VCP, 0, (LPARAM) res);
            }
        } else if (msg.message == WM_APP_REQ_REFRESH) {
            EnterCriticalSection(&g_mon_lock);
            mon_exit_locked();
            mon_init_locked();
            const DWORD cnt = g_mon_cnt;
            LeaveCriticalSection(&g_mon_lock);
            PostMessage(g_main, WM_APP_DONE_REFRESH, cnt, 0);
        }
    }

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP_REQ_SET_VCP && msg.lParam) free((void *) msg.lParam);
    }

    EnterCriticalSection(&g_mon_lock);
    mon_exit_locked();
    LeaveCriticalSection(&g_mon_lock);
    return 0;
}

static void set_slider_enabled(const BOOL on) {
    if (IsWindow(g_slider)) EnableWindow(g_slider, on);
}

static void set_cur_mon(const DWORD idx) {
    g_cur_mon = idx;
    g_cur_pct_valid = FALSE;
    EnterCriticalSection(&g_mon_lock);
    if (idx < g_mon_cnt) (void) StringCchCopyW(g_cur_device_path, _countof(g_cur_device_path), g_monitors[idx].device_path);
    else g_cur_device_path[0] = L'\0';
    LeaveCriticalSection(&g_mon_lock);
}

static void request_get_pct(const DWORD mon) {
    if (!g_worker_tid) return;

    EnterCriticalSection(&g_mon_lock);
    const BOOL valid = mon < g_mon_cnt;
    LeaveCriticalSection(&g_mon_lock);

    g_cur_pct_valid = FALSE;
    set_slider_enabled(FALSE);
    if (!valid) return;

    g_get_seq++;
    PostThreadMessage(g_worker_tid, WM_APP_REQ_GET_VCP, mon, g_get_seq);
}

static void request_set_pct(const DWORD mon, const DWORD pct) {
    if (!g_worker_tid) return;
    g_get_seq++;

    VCP_REQUEST *req = malloc(sizeof(*req));
    if (!req) return;
    req->mon_index = mon;
    req->pct = pct;
    if (!PostThreadMessage(g_worker_tid, WM_APP_REQ_SET_VCP, 0, (LPARAM) req)) free(req);
}

static void rebuild_combo_from_mons(void) {
    if (!IsWindow(g_combo)) return;
    SendMessage(g_combo, CB_RESETCONTENT, 0, 0);

    EnterCriticalSection(&g_mon_lock);
    const DWORD cnt = g_mon_cnt;
    WCHAR labels[MAX_MON][MON_LABEL_CCH];
    for (DWORD i = 0; i < cnt; i++) (void) StringCchCopyW(labels[i], MON_LABEL_CCH, g_monitors[i].display_label);
    LeaveCriticalSection(&g_mon_lock);

    if (g_cur_mon >= cnt) g_cur_mon = 0;
    for (DWORD i = 0; i < cnt; i++)
        SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM) labels[i]);
    if (cnt > 0)
        SendMessage(g_combo, CB_SETCURSEL, g_cur_mon, 0);
    else
        SendMessage(g_combo, CB_SETCURSEL, (WPARAM) -1, 0);
}

static void set_label_pct(const DWORD pct) {
    if (!IsWindow(g_label)) return;
    WCHAR buf[32];
    (void) StringCchPrintf(buf, ARRAYSIZE(buf), L"%lu%%", pct);
    SetWindowText(g_label, buf);
}

static void sync_slider_label(void) {
    if (!IsWindow(g_slider)) return;

    EnterCriticalSection(&g_mon_lock);
    const BOOL has = g_mon_cnt > 0;
    LeaveCriticalSection(&g_mon_lock);

    if (has) {
        SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
        set_label_pct(g_cur_pct);
    } else {
        SendMessage(g_slider, TBM_SETPOS, TRUE, 0);
        if (IsWindow(g_label))
            SetWindowText(g_label, L"No monitor");
    }
}

static int slider_pos_from_x(HWND wnd, int x) {
    RECT ch, th;
    SendMessage(wnd, TBM_GETCHANNELRECT, 0, (LPARAM) &ch);
    SendMessage(wnd, TBM_GETTHUMBRECT, 0, (LPARAM) &th);

    const int thumb_w = th.right - th.left;
    const int usable = ch.right - ch.left - thumb_w;
    const int vmin = (int) SendMessage(wnd, TBM_GETRANGEMIN, 0, 0);
    const int vmax = (int) SendMessage(wnd, TBM_GETRANGEMAX, 0, 0);
    if (usable <= 0) return vmin;

    int cx = x - ch.left - thumb_w / 2;
    cx = cx < 0 ? 0 : cx > usable ? usable : cx;
    return vmin + (vmax - vmin) * cx / usable;
}

static LRESULT CALLBACK slider_subclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void) id;
    (void) ref;

    switch (msg) {
        case WM_LBUTTONDOWN: {
            const POINT pt = {.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)};
            RECT th;
            SendMessage(wnd, TBM_GETTHUMBRECT, 0, (LPARAM) &th);
            if (PtInRect(&th, pt)) {
                g_slider_drag = FALSE;
                break;
            }

            const int pos = slider_pos_from_x(wnd, pt.x);
            SendMessage(wnd, TBM_SETPOS, TRUE, pos);
            SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM) wnd);
            SetCapture(wnd);
            g_slider_drag = TRUE;
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g_slider_drag) {
                const int pos = slider_pos_from_x(wnd, GET_X_LPARAM(lp));
                SendMessage(wnd, TBM_SETPOS, TRUE, pos);
                SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM) wnd);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (g_slider_drag) {
                g_slider_drag = FALSE;
                ReleaseCapture();
                const int pos = (int) SendMessage(wnd, TBM_GETPOS, 0, 0);
                SendMessage(GetParent(wnd), WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, pos), (LPARAM) wnd);
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            g_slider_drag = FALSE;
            break;

        default:
            break;
    }

    return DefSubclassProc(wnd, msg, wp, lp);
}

static LRESULT CALLBACK slider_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_combo = CreateWindow(L"COMBOBOX", NULL,
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                   10, 10, 280, 200, wnd, (HMENU) 1, NULL, NULL);
            g_label = CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
                                   10, 50, 280, 20, wnd, NULL, NULL, NULL);
            g_slider = CreateWindow(TRACKBAR_CLASS, NULL,
                                    WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                    10, 80, 280, 30, wnd, NULL, NULL, NULL);
            SendMessage(g_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
            SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
            SetWindowSubclass(g_slider, slider_subclass, 1, 0);
            CreateWindow(L"BUTTON", L"Exit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         210, 115, 80, 25, wnd, (HMENU) 2, NULL, NULL);
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == 1 && HIWORD(wp) == CBN_SELCHANGE) {
                const DWORD idx = (DWORD) SendMessage(g_combo, CB_GETCURSEL, 0, 0);
                set_cur_mon(idx);
                request_get_pct(g_cur_mon);
            } else if (LOWORD(wp) == 2) {
                PostQuitMessage(0);
            }
            break;

        case WM_HSCROLL:
            if ((HWND) lp == g_slider) {
                const DWORD code = LOWORD(wp);
                const DWORD pos = (DWORD) SendMessage(g_slider, TBM_GETPOS, 0, 0);
                set_label_pct(pos);
                if ((code == TB_ENDTRACK || code == TB_THUMBPOSITION || code == TB_PAGEUP || code == TB_PAGEDOWN || code == TB_LINEUP ||
                     code == TB_LINEDOWN) && (pos != g_cur_pct || !g_cur_pct_valid)) {
                    request_set_pct(g_cur_mon, pos);
                    g_cur_pct = pos;
                }
            }
            break;

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(wnd)) SetTimer(wnd, TIMER_HIDE_DELAY, 100, NULL);
            else KillTimer(wnd, TIMER_HIDE_DELAY);
            break;

        case WM_TIMER:
            if (wp == TIMER_HIDE_DELAY) {
                KillTimer(wnd, TIMER_HIDE_DELAY);
                if (IsWindow(g_combo) && SendMessage(g_combo, CB_GETDROPPEDSTATE, 0, 0)) return 0;
                if (GetForegroundWindow() != wnd) ShowWindow(wnd, SW_HIDE);
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

static void show_window(void) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    RECT rc;

    if (hmon && GetMonitorInfo(hmon, &mi)) rc = mi.rcWork;
    else
        SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);

    const int w = 300;
    const int h = 155;
    int x = pt.x - w / 2;
    int y = pt.y - h - 20;
    if (x < rc.left) x = rc.left;
    if (x + w > rc.right) x = rc.right - w;
    if (y < rc.top) y = pt.y + 20;
    if (y + h > rc.bottom) y = rc.bottom - h;

    if (!IsWindow(g_win)) {
        g_win = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"br_slider", NULL, WS_POPUP | WS_BORDER, x, y, w, h, NULL, NULL,
                               GetModuleHandle(NULL), NULL);
        if (!g_win) return;
    } else {
        SetWindowPos(g_win, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    rebuild_combo_from_mons();
    sync_slider_label();
    request_get_pct(g_cur_mon);
    ShowWindow(g_win, SW_SHOW);
    SetForegroundWindow(g_win);
}

static void hide_window(void) {
    if (IsWindow(g_win) && IsWindowVisible(g_win)) ShowWindow(g_win, SW_HIDE);
}

static void schedule_rescan(const DWORD delay, const int retries) {
    if (retries > g_retry_budget) g_retry_budget = retries;
    if (!g_main) return;
    KillTimer(g_main, TIMER_RESCAN_RETRY);
    SetTimer(g_main, TIMER_RESCAN, delay, NULL);
}

static LRESULT CALLBACK main_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbar_created_msg && msg == g_taskbar_created_msg) {
        Shell_NotifyIcon(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
        case WM_USER + 1:
            if (lp == WM_LBUTTONDBLCLK) return 0;
            if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) {
                if (IsWindow(g_win)) KillTimer(g_win, TIMER_HIDE_DELAY);
                const ULONGLONG now = GetTickCount64();
                if (now - g_last_toggle_tick < GetDoubleClickTime()) return 0;
                g_last_toggle_tick = now;
                if (IsWindow(g_win) && IsWindowVisible(g_win)) hide_window();
                else show_window();
            }
            return 0;

        case WM_APP_SET_FAILED: {
            const DWORD idx = (DWORD) wp;
            if (idx != g_cur_mon) return 0;
            g_cur_pct_valid = FALSE;
            if (!g_slider_drag) request_get_pct(g_cur_mon);
            return 0;
        }

        case WM_APP_DONE_GET_VCP: {
            VCP_RESULT *res = (VCP_RESULT *) lp;
            if (res) {
                if (res->seq == g_get_seq) {
                    if (res->ok) {
                        g_cur_pct = res->pct;
                        g_cur_pct_valid = TRUE;
                        sync_slider_label();
                    } else {
                        g_cur_pct_valid = FALSE;
                    }

                    EnterCriticalSection(&g_mon_lock);
                    const BOOL has = g_mon_cnt > 0;
                    LeaveCriticalSection(&g_mon_lock);
                    set_slider_enabled(has);
                }
                free(res);
            }
            return 0;
        }

        case WM_APP_DONE_REFRESH: {
            const DWORD cnt = (DWORD) wp;
            if (cnt > 0) {
                g_retry_budget = 0;
                DWORD new_idx = 0;
                EnterCriticalSection(&g_mon_lock);

                if (g_cur_device_path[0]) {
                    for (DWORD i = 0; i < g_mon_cnt; i++) {
                        if (_wcsicmp(g_monitors[i].device_path, g_cur_device_path) == 0) {
                            new_idx = i;
                            break;
                        }
                    }
                } else if (g_cur_mon < g_mon_cnt) {
                    new_idx = g_cur_mon;
                }

                (void) StringCchCopyW(g_cur_device_path, _countof(g_cur_device_path), g_monitors[new_idx].device_path);
                LeaveCriticalSection(&g_mon_lock);

                g_cur_mon = new_idx;
                g_cur_pct_valid = FALSE;
                if (IsWindow(g_win) && IsWindowVisible(g_win)) {
                    rebuild_combo_from_mons();
                    sync_slider_label();
                }
                request_get_pct(g_cur_mon);
            } else {
                g_cur_mon = 0;
                g_cur_device_path[0] = L'\0';
                g_cur_pct_valid = FALSE;
                set_slider_enabled(FALSE);

                if (IsWindow(g_win) && IsWindowVisible(g_win)) {
                    rebuild_combo_from_mons();
                    sync_slider_label();
                }

                if (g_retry_budget > 0) {
                    g_retry_budget--;
                    KillTimer(wnd, TIMER_RESCAN);
                    SetTimer(wnd, TIMER_RESCAN_RETRY, 2500, NULL);
                }
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == TIMER_RESCAN || wp == TIMER_RESCAN_RETRY) {
                KillTimer(wnd, wp);
                if (g_worker_tid)
                    PostThreadMessage(g_worker_tid, WM_APP_REQ_REFRESH, 0, 0);
                return 0;
            }
            break;

        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                const DEV_BROADCAST_HDR *hdr = (const DEV_BROADCAST_HDR *) lp;
                if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE && IsEqualGUID(
                        &((const DEV_BROADCAST_DEVICEINTERFACE_W *)hdr)->dbcc_classguid, &GUID_DEVINTERFACE_MONITOR)) {
                    const ULONGLONG now = GetTickCount64();
                    if (now - g_last_devchange_tick < 800) return TRUE;
                    g_last_devchange_tick = now;
                    schedule_rescan(500, 2);
                    return TRUE;
                }
            }
            break;

        case WM_DISPLAYCHANGE:
            schedule_rescan(500, 2);
            return 0;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                schedule_rescan(1500, 2);
                return TRUE;
            }
            break;

        default:
            break;
    }

    return DefWindowProc(wnd, msg, wp, lp);
}

int WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void) prev;
    (void) cmd;
    (void) show;

    InitializeCriticalSection(&g_mon_lock);
    g_worker_ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    const INITCOMMONCONTROLSEX icc = {.dwSize = sizeof(icc), .dwICC = ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEX wc = {.cbSize = sizeof(wc)};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"br_main";
    RegisterClassEx(&wc);

    wc.lpfnWndProc = slider_proc;
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = L"br_slider";
    RegisterClassEx(&wc);

    g_worker_thread = CreateThread(NULL, 0, vcp_worker, NULL, 0, NULL);
    if (!g_worker_thread) {
        MessageBoxW(NULL, L"Failed to create DDC worker thread.", L"Error", MB_ICONERROR);
        DeleteCriticalSection(&g_mon_lock);
        CloseHandle(g_worker_ready_event);
        return 1;
    }

    if (WaitForSingleObject(g_worker_ready_event, 2000) != WAIT_OBJECT_0) {
        MessageBoxW(NULL, L"DDC worker failed to start.", L"Error", MB_ICONERROR);
        CloseHandle(g_worker_thread);
        DeleteCriticalSection(&g_mon_lock);
        CloseHandle(g_worker_ready_event);
        return 1;
    }

    g_main = CreateWindowEx(WS_EX_TOOLWINDOW, L"br_main", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_main) {
        MessageBoxW(NULL, L"Failed to create main message window.", L"Error", MB_ICONERROR);
        PostThreadMessage(g_worker_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_worker_thread, INFINITE);
        CloseHandle(g_worker_thread);
        DeleteCriticalSection(&g_mon_lock);
        CloseHandle(g_worker_ready_event);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W filter;
    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_MONITOR;
    g_mon_notify = RegisterDeviceNotificationW(g_main, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_taskbar_created_msg = RegisterWindowMessage(L"TaskbarCreated");

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_USER + 1;
    g_nid.hIcon = (HICON) LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    if (g_nid.hIcon) {
        g_nid_icon_owned = TRUE;
    } else {
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        g_nid_icon_owned = FALSE;
    }

    (void) StringCchCopy(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"brightness");
    Shell_NotifyIcon(NIM_ADD, &g_nid);

    g_retry_budget = 2;
    PostThreadMessage(g_worker_tid, WM_APP_REQ_REFRESH, 0, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_worker_thread) {
        PostThreadMessage(g_worker_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_worker_thread, INFINITE);
        CloseHandle(g_worker_thread);
        g_worker_thread = NULL;
    }

    MSG drain;
    while (PeekMessage(&drain, g_main, WM_APP_DONE_GET_VCP, WM_APP_DONE_GET_VCP, PM_REMOVE)) free((void *) drain.lParam);
    while (PeekMessage(&drain, g_main, WM_APP_SET_FAILED, WM_APP_SET_FAILED, PM_REMOVE)) {
    }
    while (PeekMessage(&drain, g_main, WM_APP_DONE_REFRESH, WM_APP_DONE_REFRESH, PM_REMOVE)) {
    }

    if (g_mon_notify) {
        UnregisterDeviceNotification(g_mon_notify);
        g_mon_notify = NULL;
    }

    if (IsWindow(g_win)) DestroyWindow(g_win);
    if (IsWindow(g_main)) DestroyWindow(g_main);

    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    if (g_nid_icon_owned && g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = NULL;
        g_nid_icon_owned = FALSE;
    }

    DeleteCriticalSection(&g_mon_lock);
    CloseHandle(g_worker_ready_event);
    return 0;
}
