#include <windows.h>
#include <commctrl.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <strsafe.h>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows /entry:WinMainCRTStartup")

#define MAX_MON 8

static HWND g_main_wnd;
static HWND g_slider_wnd;
static HWND g_slider;
static HWND g_label;
static HWND g_combo;
static HMENU g_menu;
static NOTIFYICONDATA g_nid;

static DWORD g_cur_pct = 50;
static BOOL  g_dragging;

static PHYSICAL_MONITOR g_mons[MAX_MON];
static DWORD g_mon_cnt;
static DWORD g_cur_mon;

static DWORD g_vcp_max = 100;

static LRESULT CALLBACK main_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK slider_proc(HWND, UINT, WPARAM, LPARAM);
static BOOL CALLBACK enum_proc(HMONITOR, HDC, LPRECT, LPARAM);

static BOOL  mon_init(void);
static void  mon_exit(void);
static void  mon_set_pct(DWORD);
static void  ui_show_slider(void);
static void  ui_update_label(DWORD);

int WINAPI WinMain(HINSTANCE inst, HINSTANCE a, LPSTR b, int c)
{
    WNDCLASSEX wc;
    MSG msg;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"br_main";
    RegisterClassEx(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = slider_proc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"br_slider";
    RegisterClassEx(&wc);

    mon_init();

    g_main_wnd = CreateWindowEx(
        0, L"br_main", L"",
        WS_OVERLAPPED, 0, 0, 0, 0,
        NULL, NULL, inst, NULL);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_USER + 1;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    StringCchCopy(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"brightness");
    Shell_NotifyIcon(NIM_ADD, &g_nid);

    g_menu = CreatePopupMenu();
    AppendMenu(g_menu, MF_STRING, 1, L"exit");

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    DestroyMenu(g_menu);
    DestroyIcon(g_nid.hIcon);
    mon_exit();

    return 0;
}

static LRESULT CALLBACK main_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_USER + 1) {
        if (lp == WM_LBUTTONUP)
            ui_show_slider();
        else if (lp == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(wnd);
            TrackPopupMenu(g_menu, TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, wnd, NULL);
        }
        return 0;
    }

    if (msg == WM_COMMAND && LOWORD(wp) == 1) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(wnd, msg, wp, lp);
}

static LRESULT CALLBACK slider_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
    {
        g_combo = CreateWindow(
            L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            10, 10, 280, 200,
            wnd, (HMENU)1, NULL, NULL);

        for (DWORD i = 0; i < g_mon_cnt; i++) {
            WCHAR buf[64];
            wsprintf(buf, L"Monitor %lu", i + 1);
            SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)buf);
        }

        SendMessage(g_combo, CB_SETCURSEL, 0, 0);

        g_label = CreateWindow(
            L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            10, 50, 280, 20,
            wnd, NULL, NULL, NULL);

        g_slider = CreateWindow(
            TRACKBAR_CLASS, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ,
            10, 80, 280, 30,
            wnd, NULL, NULL, NULL);

        SendMessage(g_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));

        if (g_mon_cnt > 0) {
            DWORD cur, max;
            if (GetVCPFeatureAndVCPFeatureReply(
                g_mons[0].hPhysicalMonitor,
                0x10, NULL, &cur, &max))
            {
                g_vcp_max = max ? max : 100;
                g_cur_pct = cur * 100 / g_vcp_max;
            }
        }

        SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
        ui_update_label(g_cur_pct);
    }
    break;

    case WM_COMMAND:
        if (LOWORD(wp) == 1 && HIWORD(wp) == CBN_SELCHANGE) {
            g_cur_mon = SendMessage(g_combo, CB_GETCURSEL, 0, 0);

            DWORD cur, max;

            if (GetVCPFeatureAndVCPFeatureReply(
                g_mons[g_cur_mon].hPhysicalMonitor,
                0x10, NULL, &cur, &max))
            {
                g_vcp_max = max ? max : 100;
                g_cur_pct = cur * 100 / g_vcp_max;

                SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
                ui_update_label(g_cur_pct);
            }
        }
        break;

    case WM_HSCROLL:
        if ((HWND)lp == g_slider) {
            DWORD pos = (DWORD)SendMessage(g_slider, TBM_GETPOS, 0, 0);
            ui_update_label(pos);

            if (LOWORD(wp) == TB_THUMBTRACK)
                g_dragging = TRUE;
            else {
                if (g_dragging && pos != g_cur_pct)
                    mon_set_pct(pos);
                g_dragging = FALSE;
            }
        }
        break;

    case WM_CLOSE:
        DestroyWindow(wnd);
        g_slider_wnd = NULL;
        break;

    default:
        return DefWindowProc(wnd, msg, wp, lp);
    }

    return 0;
}

static void ui_show_slider(void)
{
    int w = 320;
    int h = 150;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    if (g_slider_wnd && IsWindow(g_slider_wnd))
        return;

    g_slider_wnd = CreateWindowEx(
        0, L"br_slider", L"brightness",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (sw - w) / 2,
        (sh - h) / 2,
        w, h,
        NULL, NULL, NULL, NULL);
}

static void ui_update_label(DWORD pct)
{
    WCHAR buf[32];
    StringCchPrintf(buf, ARRAYSIZE(buf), L"%lu%%", pct);
    SetWindowText(g_label, buf);
}

static BOOL CALLBACK enum_proc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    DWORD cnt;

    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &cnt) || cnt == 0)
        return TRUE;

    PHYSICAL_MONITOR mons[8];

    if (GetPhysicalMonitorsFromHMONITOR(hm, cnt, mons)) {
        for (DWORD i = 0; i < cnt && g_mon_cnt < MAX_MON; i++) {

            DWORD cur, max;

            if (GetVCPFeatureAndVCPFeatureReply(
                mons[i].hPhysicalMonitor,
                0x10, NULL, &cur, &max))
            {
                g_mons[g_mon_cnt++] = mons[i];
            }
            else {
                DestroyPhysicalMonitor(mons[i].hPhysicalMonitor);
            }
        }
    }

    return TRUE;
}

static BOOL mon_init(void)
{
    g_mon_cnt = 0;

    EnumDisplayMonitors(NULL, NULL, enum_proc, 0);

    g_cur_mon = 0;
    return g_mon_cnt > 0;
}

static void mon_exit(void)
{
    for (DWORD i = 0; i < g_mon_cnt; i++) {
        DestroyPhysicalMonitor(g_mons[i].hPhysicalMonitor);
    }
}

static void mon_set_pct(DWORD pct)
{
    if (g_cur_mon >= g_mon_cnt)
        return;

    if (pct > 100)
        pct = 100;

    DWORD vcp = pct * g_vcp_max / 100;

    SetVCPFeature(
        g_mons[g_cur_mon].hPhysicalMonitor,
        0x10,
        vcp);

    g_cur_pct = pct;
}