#include "ui/dialogs.h"
#include "ui/helpers.h"
#include <commdlg.h>
#include <shlobj.h>
#include <string>

// overlay HWND (defined in main.cpp)
extern HWND g_overlayHwnd;

static void hideOverlay() {
    if (g_overlayHwnd) ShowWindow(g_overlayHwnd, SW_HIDE);
}
static void showOverlay() {
    if (g_overlayHwnd) ShowWindow(g_overlayHwnd, SW_SHOW);
}

std::string openFileDialog(HWND hwnd) {
    hideOverlay();
    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        L"Video\0*.mp4;*.avi;*.mkv;*.mov;*.flv;*.wmv;*.rmvb;*.rm;*.3gp;*.mpg;*.mpeg;*.webm;*.ts;*.m2ts\0"
        L"All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    std::string result;
    if (GetOpenFileNameW(&ofn)) result = WideToUtf8(file);
    showOverlay();
    return result;
}

std::string openSubtitleDialog(HWND hwnd) {
    hideOverlay();
    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        L"Subtitle\0*.srt;*.ass;*.ssa;*.sub;*.idx;*.sup\0"
        L"All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    std::string result;
    if (GetOpenFileNameW(&ofn)) result = WideToUtf8(file);
    showOverlay();
    return result;
}

std::string openUrlDialog(HWND hwnd) {
    hideOverlay();
    struct Ctx { bool ok; std::string url; } ctx{false, ""};

    RECT rc; GetWindowRect(hwnd, &rc);
    int dw = 440, dh = 100;
    int cx = (rc.left + rc.right - dw) / 2;
    int cy = (rc.top + rc.bottom - dh) / 2;
    int pad = 10, labelW = 40, editW = 320, btnW = 60;
    int editH = 22, btnH = 24;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = [](HWND w, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
        auto* c = reinterpret_cast<Ctx*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        switch (m) {
        case WM_COMMAND:
            if (LOWORD(wp) == 1 && c) {
                wchar_t buf[2048] = {};
                GetWindowTextW(GetDlgItem(w, 1002), buf, 2048);
                c->ok = true; c->url = WideToUtf8(buf);
                DestroyWindow(w);
            }
            if (LOWORD(wp) == 2 && c) {
                DestroyWindow(w);
            }
            return 0;
        case WM_CLOSE:
            if (c) DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, m, wp, lp);
    };
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PhantomUrlInput";
    wc.hCursor = LoadCursorW(NULL, L"IDC_ARROW");
    RegisterClassExW(&wc);

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"PhantomUrlInput", L"Open URL",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        cx, cy, dw, dh, hwnd, 0, GetModuleHandleW(0), 0);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)&ctx);

    HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    HWND hLabel = CreateWindowExW(0, L"STATIC", L"URL:",
        WS_CHILD | WS_VISIBLE, pad, pad + 2, labelW, 20, dlg, 0, 0, 0);
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"https://",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        pad + labelW, pad, editW, editH, dlg, (HMENU)1002, 0, 0);
    HWND hOk = CreateWindowExW(0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        dw - pad - btnW * 2 - 8, dh - pad - btnH, btnW, btnH, dlg, (HMENU)1, 0, 0);
    HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        dw - pad - btnW, dh - pad - btnH, btnW, btnH, dlg, (HMENU)2, 0, 0);

    SendMessageW(hLabel, WM_SETFONT, (WPARAM)hf, TRUE);
    SendMessageW(hEdit, WM_SETFONT, (WPARAM)hf, TRUE);
    SendMessageW(hOk, WM_SETFONT, (WPARAM)hf, TRUE);
    SendMessageW(hCancel, WM_SETFONT, (WPARAM)hf, TRUE);

    SendMessageW(hEdit, EM_SETSEL, 0, -1);
    SetFocus(hEdit);
    ShowWindow(dlg, SW_SHOW);

    MSG m;
    while (IsWindow(dlg)) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) break;
            if (!IsDialogMessageW(dlg, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        if (IsWindow(dlg)) WaitMessage();
    }
    showOverlay();
    return ctx.url;
}

std::string openFolderDialog(HWND hwnd) {
    hideOverlay();
    std::string result;
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coOwned = SUCCEEDED(coInit);
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Select folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH * 2] = {};
        if (SHGetPathFromIDListW(pidl, path)) result = WideToUtf8(path);
        CoTaskMemFree(pidl);
    }
    if (coOwned) CoUninitialize();
    showOverlay();
    return result;
}
