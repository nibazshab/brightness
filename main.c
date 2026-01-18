#include <windows.h>
#include <commctrl.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <strsafe.h>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows /entry:WinMainCRTStartup")

static HWND g_main_wnd;
static HWND g_slider_wnd;
static HWND g_slider;
static HWND g_label;
static HMENU g_menu;
static NOTIFYICONDATA g_nid;

static DWORD g_cur_pct = 50;
static BOOL  g_dragging;

static PHYSICAL_MONITOR g_mon;
static BOOL  g_mon_ok;
static DWORD g_vcp_max = 100;

static LRESULT CALLBACK main_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK slider_proc(HWND, UINT, WPARAM, LPARAM);

static BOOL  mon_init(void);
static void  mon_exit(void);
static void  mon_set_pct(DWORD);
static void  ui_show_slider(void);
static void  ui_update_label(DWORD);

int WINAPI WinMain(
	_In_ HINSTANCE inst,
	_In_opt_ HINSTANCE a,
	_In_ LPSTR b,
	_In_ int c)
{
	WNDCLASSEX wc;
	MSG msg;

	INITCOMMONCONTROLSEX icc = {
		sizeof(icc),
		ICC_BAR_CLASSES
	};
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
		g_label = CreateWindow(
			L"STATIC", L"",
			WS_CHILD | WS_VISIBLE | SS_CENTER,
			10, 10, 280, 20,
			wnd, NULL, NULL, NULL);

		g_slider = CreateWindow(
			TRACKBAR_CLASS, NULL,
			WS_CHILD | WS_VISIBLE | TBS_HORZ,
			10, 40, 280, 30,
			wnd, NULL, NULL, NULL);

		SendMessage(g_slider, TBM_SETRANGE, TRUE,
			MAKELONG(0, 100));
		SendMessage(g_slider, TBM_SETPOS, TRUE, g_cur_pct);
		ui_update_label(g_cur_pct);
		break;

	case WM_HSCROLL:
		if ((HWND)lp == g_slider) {
			DWORD pos;

			pos = (DWORD)SendMessage(g_slider,
				TBM_GETPOS, 0, 0);
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
	int h = 120;
	int sw, sh;

	if (g_slider_wnd && IsWindow(g_slider_wnd))
		return;

	sw = GetSystemMetrics(SM_CXSCREEN);
	sh = GetSystemMetrics(SM_CYSCREEN);

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

	StringCchPrintf(buf, ARRAYSIZE(buf),
		L"%lu%%", pct);
	SetWindowText(g_label, buf);
}

static BOOL mon_init(void)
{
	HMONITOR hm;
	DWORD cnt;
	DWORD cur, max;

	if (g_mon_ok)
		return TRUE;

	hm = MonitorFromPoint((POINT) { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

	if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &cnt) ||
		cnt == 0)
		return FALSE;

	if (!GetPhysicalMonitorsFromHMONITOR(hm, 1, &g_mon))
		return FALSE;

	if (!GetVCPFeatureAndVCPFeatureReply(
		g_mon.hPhysicalMonitor,
		0x10, NULL, &cur, &max)) {
		mon_exit();
		return FALSE;
	}

	g_vcp_max = max ? max : 100;
	g_cur_pct = cur * 100 / g_vcp_max;
	g_mon_ok = TRUE;

	return TRUE;
}

static void mon_exit(void)
{
	if (g_mon_ok) {
		DestroyPhysicalMonitors(1, &g_mon);
		g_mon_ok = FALSE;
	}
}

static void mon_set_pct(DWORD pct)
{
	DWORD vcp;

	if (!g_mon_ok)
		return;

	if (pct > 100)
		pct = 100;

	vcp = pct * g_vcp_max / 100;
	SetVCPFeature(g_mon.hPhysicalMonitor, 0x10, vcp);
	g_cur_pct = pct;
}
