#include <commctrl.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <strsafe.h>
#include <windows.h>

#include "resource.h"

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows /entry:WinMainCRTStartup")

#define MAX_MON 8

static HWND g_main, g_win, g_slider, g_label, g_combo;
static HMENU g_menu;
static NOTIFYICONDATA g_nid;

static PHYSICAL_MONITOR g_mons[MAX_MON];
static DWORD g_mon_cnt, g_cur_mon;

static DWORD g_cur_pct = 50;
static DWORD g_vcp_max = 100;

static BOOL mon_get_pct(const DWORD i, DWORD *pct)
{
    DWORD cur, max;

    if (i >= g_mon_cnt)
        return FALSE;

    if (!GetVCPFeatureAndVCPFeatureReply(g_mons[i].hPhysicalMonitor, 0x10, NULL, &cur, &max))
        return FALSE;

    max = max ? max : 100;
    g_vcp_max = max;
    *pct = cur * 100 / max;

    return TRUE;
}

static void mon_set_pct(DWORD pct)
{
    if (g_cur_mon >= g_mon_cnt)
        return;

    if (pct > 100)
        pct = 100;

    const DWORD vcp = pct * g_vcp_max / 100;

    SetVCPFeature(g_mons[g_cur_mon].hPhysicalMonitor, 0x10, vcp);

    g_cur_pct = pct;
}

static BOOL CALLBACK enum_proc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    DWORD cnt;

    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &cnt) || !cnt)
        return TRUE;

    PHYSICAL_MONITOR mons[MAX_MON];

    if (GetPhysicalMonitorsFromHMONITOR(hm, cnt, mons)) {
        for (DWORD i = 0; i < cnt && g_mon_cnt < MAX_MON; i++) {
            DWORD cur, max;

            if (GetVCPFeatureAndVCPFeatureReply(mons[i].hPhysicalMonitor, 0x10, NULL, &cur, &max)) {
                g_mons[g_mon_cnt++] = mons[i];
            }
            else {
                DestroyPhysicalMonitor(mons[i].hPhysicalMonitor);
            }
        }
    }

    return TRUE;
}

static void mon_init(void)
{
    g_mon_cnt = 0;
    EnumDisplayMonitors(NULL, NULL, enum_proc, 0);
    g_cur_mon = 0;
}

static void mon_exit(void)
{
    for (DWORD i = 0; i < g_mon_cnt; i++) {
        if (g_mons[i].hPhysicalMonitor)
            DestroyPhysicalMonitor(g_mons[i].hPhysicalMonitor);
    }

    g_mon_cnt = 0;
}

static void refresh_monitors(void)
{
    mon_exit();
    mon_init();

    if (IsWindow(g_combo)) {
        SendMessage(g_combo, CB_RESETCONTENT, 0, 0);

        for (DWORD i = 0; i < g_mon_cnt; i++) {
            SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)(LPCWSTR)g_mons[i].szPhysicalMonitorDescription);
        }

        SendMessage(g_combo, CB_SETCURSEL, 0, 0);
        g_cur_mon = 0;
    }
}

static LRESULT CALLBACK slider_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CREATE:
            g_combo = CreateWindow(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 10, 10, 280, 200, wnd,
                                   (HMENU)1, NULL, NULL);

            for (DWORD i = 0; i < g_mon_cnt; i++) {
                SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)(LPCWSTR)g_mons[i].szPhysicalMonitorDescription);
            }

            SendMessage(g_combo, CB_SETCURSEL, 0, 0);

            g_label =
                CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 50, 280, 20, wnd, NULL, NULL, NULL);

            g_slider = CreateWindow(TRACKBAR_CLASS, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ, 10, 80, 280, 30, wnd, NULL,
                                    NULL, NULL);

            SendMessage(g_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));

            if (mon_get_pct(0, &g_cur_pct)) {
                SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);

                WCHAR buf[32];
                (void)StringCchPrintf(buf, ARRAYSIZE(buf), L"%lu%%", g_cur_pct);
                SetWindowText(g_label, buf);
            }

            break;

        case WM_COMMAND:
            if (LOWORD(wp) == 1 && HIWORD(wp) == CBN_SELCHANGE) {
                g_cur_mon = SendMessage(g_combo, CB_GETCURSEL, 0, 0);

                if (mon_get_pct(g_cur_mon, &g_cur_pct)) {
                    SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);

                    WCHAR buf[32];
                    (void)StringCchPrintf(buf, ARRAYSIZE(buf), L"%lu%%", g_cur_pct);
                    SetWindowText(g_label, buf);
                }
            }

            break;

        case WM_HSCROLL:
            if ((HWND)lp == g_slider) {
                const DWORD code = LOWORD(wp);
                const DWORD pos = SendMessage(g_slider, TBM_GETPOS, 0, 0);

                WCHAR buf[32];
                (void)StringCchPrintf(buf, ARRAYSIZE(buf), L"%lu%%", pos);
                SetWindowText(g_label, buf);

                if (code == TB_ENDTRACK || code == TB_THUMBPOSITION || code == TB_PAGEUP || code == TB_PAGEDOWN ||
                    code == TB_LINEUP || code == TB_LINEDOWN) {
                    if (pos != g_cur_pct)
                        mon_set_pct(pos);
                }
            }

            break;

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                PostMessage(wnd, WM_CLOSE, 0, 0);
            }

            break;

        case WM_CLOSE:
            DestroyWindow(wnd);
            g_win = NULL;
            break;

        default:
            return DefWindowProc(wnd, msg, wp, lp);
    }

    return 0;
}

static void show_window(void)
{
    if (IsWindow(g_win))
        return;

    const int w = 300;
    const int h = 120;

    POINT pt;
    GetCursorPos(&pt);

    RECT rc = {0};
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);

    int x = pt.x - w / 2;
    int y = pt.y - h - 20;

    if (x < rc.left)
        x = rc.left;

    if (x + w > rc.right)
        x = rc.right - w;

    if (y < rc.top) {
        y = pt.y + 20;
    }

    if (y + h > rc.bottom)
        y = rc.bottom - h;

    g_win = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"br_slider", NULL, WS_POPUP | WS_BORDER, x, y, w, h, NULL,
                           NULL, GetModuleHandle(NULL), NULL);

    if (g_win) {
        ShowWindow(g_win, SW_SHOW);
        SetForegroundWindow(g_win);
    }
}

static LRESULT CALLBACK main_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_USER + 1:
            if (lp == WM_LBUTTONUP)
                show_window();
            else if (lp == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(wnd);
                TrackPopupMenu(g_menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, NULL);
            }
            return 0;

        case WM_DISPLAYCHANGE:
            refresh_monitors();
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == 1) {
                PostQuitMessage(0);
                return 0;
            }
            break;
        default:;
    }

    return DefWindowProc(wnd, msg, wp, lp);
}

int WinMain(HINSTANCE inst, HINSTANCE a, LPSTR b, int c)
{
    const INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEX wc = {sizeof(wc)};

    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"br_main";
    RegisterClassEx(&wc);

    wc.lpfnWndProc = slider_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"br_slider";
    RegisterClassEx(&wc);

    mon_init();

    g_main = CreateWindowEx(0, L"br_main", L"", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);

    ZeroMemory(&g_nid, sizeof(g_nid));

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_USER + 1;
    g_nid.hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    (void)StringCchCopy(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"brightness");

    Shell_NotifyIcon(NIM_ADD, &g_nid);

    g_menu = CreatePopupMenu();
    AppendMenu(g_menu, MF_STRING, 1, L"exit");

    MSG msg;
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
