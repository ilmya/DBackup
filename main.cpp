// Copyright 2026 BackupTool Team. All rights reserved.

/**
 * main.cpp — Win32 API 图形界面
 *
 * 简洁备份布局：
 *   Tab 1 "打包备份": 选择源文件或目录 → 选择备份路径 → 开始备份
 *   Tab 2 "还原解包": 选择备份文件 → 选择还原目录 → 开始还原
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06000000
#endif

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <codecvt>
#include <locale>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include <cstdlib>

#include "packer.h"

#pragma comment(lib, "comctl32.lib")

// ═══════════════════ 控件 ID ═══════════════════
enum CtrlID {
    ID_TAB            = 100,
    // 打包面板
    ID_BACKUP_SRC     = 201,
    ID_BACKUP_SRC_BTN = 202,
    ID_BACKUP_DST     = 203,
    ID_BACKUP_DST_BTN = 204,
    ID_BTN_PACK       = 205,
    ID_BACKUP_PASSWORD = 206,
    ID_BACKUP_FILTER   = 207,
    ID_BACKUP_REMOVE_BTN = 208,
    // 还原面板
    ID_RESTORE_SRC     = 301,
    ID_RESTORE_SRC_BTN = 302,
    ID_RESTORE_DST     = 303,
    ID_RESTORE_DST_BTN = 304,
    ID_BTN_UNPACK      = 305,
    ID_RESTORE_PASSWORD = 306,
    // 日志
    ID_LOG = 400,
};

// ═══════════════════ 全局状态 ═══════════════════
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd        = nullptr;
static HWND g_hTab        = nullptr;
// 打包面板控件
static HWND g_hBackupSrc    = nullptr;
static HWND g_hBackupSrcBtn = nullptr;
static HWND g_hBackupRemoveBtn = nullptr;
static HWND g_hBackupDst    = nullptr;
static HWND g_hBackupDstBtn = nullptr;
static HWND g_hBtnPack      = nullptr;
static HWND g_hBackupSrcLabel = nullptr;
static HWND g_hBackupDstLabel = nullptr;
static HWND g_hBackupPasswordLabel = nullptr;
static HWND g_hBackupFilterLabel = nullptr;
static HWND g_hBackupPassword = nullptr;
static HWND g_hBackupFilter = nullptr;
// 还原面板控件
static HWND g_hRestoreSrc    = nullptr;
static HWND g_hRestoreSrcBtn = nullptr;
static HWND g_hRestoreDst    = nullptr;
static HWND g_hRestoreDstBtn = nullptr;
static HWND g_hBtnUnpack     = nullptr;
static HWND g_hRestoreSrcLabel = nullptr;
static HWND g_hRestoreDstLabel = nullptr;
static HWND g_hRestorePasswordLabel = nullptr;
static HWND g_hRestorePassword = nullptr;
// 日志
static HWND g_hLog = nullptr;
static std::vector<std::wstring> g_backupSources;

// ═══════════════════ 字符串转换 ═══════════════════

static std::string  WtoU(const std::wstring &ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring UtoW(const std::string &s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

// ═══════════════════ 工具函数 ═══════════════════

static std::wstring GetEditText(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return L"";
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(hEdit, buf.data(), len + 1);
    buf.resize(len);
    return buf;
}

static void SetEditText(HWND hEdit, const std::wstring &text) {
    SetWindowTextW(hEdit, text.c_str());
}

static void ShowBackupPanel(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    ShowWindow(g_hBackupSrcLabel, cmd);
    ShowWindow(g_hBackupDstLabel, cmd);
    ShowWindow(g_hBackupPasswordLabel, cmd);
    ShowWindow(g_hBackupFilterLabel, cmd);
    ShowWindow(g_hBackupSrc,    cmd);
    ShowWindow(g_hBackupSrcBtn, cmd);
    ShowWindow(g_hBackupRemoveBtn, cmd);
    ShowWindow(g_hBackupDst,    cmd);
    ShowWindow(g_hBackupDstBtn, cmd);
    ShowWindow(g_hBtnPack,      cmd);
    ShowWindow(g_hBackupPassword, cmd);
    ShowWindow(g_hBackupFilter, cmd);
}

static void ShowRestorePanel(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    ShowWindow(g_hRestoreSrcLabel, cmd);
    ShowWindow(g_hRestoreDstLabel, cmd);
    ShowWindow(g_hRestorePasswordLabel, cmd);
    ShowWindow(g_hRestoreSrc,    cmd);
    ShowWindow(g_hRestoreSrcBtn, cmd);
    ShowWindow(g_hRestoreDst,    cmd);
    ShowWindow(g_hRestoreDstBtn, cmd);
    ShowWindow(g_hBtnUnpack,     cmd);
    ShowWindow(g_hRestorePassword, cmd);
}

static void SwitchTab(int index) {
    if (index == 0) {
        ShowBackupPanel(true);
        ShowRestorePanel(false);
    } else {
        ShowBackupPanel(false);
        ShowRestorePanel(true);
    }
}

static bool AreBothSet(HWND h1, HWND h2) {
    return GetWindowTextLengthW(h1) > 0 && GetWindowTextLengthW(h2) > 0;
}

static bool CanStartBackup() {
    return !g_backupSources.empty() && GetWindowTextLengthW(g_hBackupDst) > 0;
}

static void RefreshBackupSources() {
    SendMessageW(g_hBackupSrc, LB_RESETCONTENT, 0, 0);
    for (const auto &source : g_backupSources) {
        SendMessageW(g_hBackupSrc, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(source.c_str()));
    }
    EnableWindow(g_hBtnPack, CanStartBackup());
}

// ═══════════════════ 日志 ═══════════════════

static void AppendLog(const std::wstring &msg, COLORREF color = RGB(0, 0, 0)) {
    // 获取当前文本长度
    int len = GetWindowTextLengthW(g_hLog);
    // 添加换行（如果不是第一行）
    std::wstring line = (len > 0 ? L"\r\n" : L"") + msg;

    // 追加文本
    SendMessageW(g_hLog, EM_SETSEL, len, len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());

    // 滚动到底部
    SendMessageW(g_hLog, EM_LINESCROLL, 0, SendMessageW(g_hLog, EM_GETLINECOUNT, 0, 0));
}

static void LogInfo(const std::wstring &msg)  { AppendLog(L"ℹ " + msg, RGB(37, 99, 235)); }
static void LogSuccess(const std::wstring &msg) { AppendLog(L"✔ " + msg, RGB(22, 163, 74)); }
static void LogError(const std::wstring &msg)  { AppendLog(L"✘ " + msg, RGB(220, 38, 38)); }

// ═══════════════════ 文件对话框 ═══════════════════

static std::wstring GetDialogPath(IFileDialog *dialog) {
    IShellItem *item = nullptr;
    if (FAILED(dialog->GetResult(&item))) return L"";
    PWSTR rawPath = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) {
        result = rawPath;
        CoTaskMemFree(rawPath);
    }
    item->Release();
    return result;
}

static std::vector<std::wstring> GetDialogPaths(IFileOpenDialog *dialog) {
    std::vector<std::wstring> paths;
    IShellItemArray *items = nullptr;
    if (FAILED(dialog->GetResults(&items))) return paths;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD index = 0; index < count; ++index) {
        IShellItem *item = nullptr;
        if (FAILED(items->GetItemAt(index, &item))) continue;
        PWSTR rawPath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) {
            paths.emplace_back(rawPath);
            CoTaskMemFree(rawPath);
        }
        item->Release();
    }
    items->Release();
    return paths;
}

static std::vector<std::wstring> BrowseForFolders(const wchar_t *title) {
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT |
                       FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    std::vector<std::wstring> result;
    if (SUCCEEDED(dialog->Show(g_hWnd))) result = GetDialogPaths(dialog);
    dialog->Release();
    return result;
}

static std::vector<std::wstring> BrowseForOpenFiles(const wchar_t *title, bool archiveOnly,
                                                     bool allowMultiple) {
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    DWORD selectionOptions = FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (allowMultiple) selectionOptions |= FOS_ALLOWMULTISELECT;
    dialog->SetOptions(options | selectionOptions);
    dialog->SetTitle(title);
    const COMDLG_FILTERSPEC archiveFilters[] = {
        {L"ABK 备份文件 (*.abk)", L"*.abk"}, {L"所有文件 (*.*)", L"*.*"}};
    if (archiveOnly) dialog->SetFileTypes(2, archiveFilters);
    std::vector<std::wstring> result;
    if (SUCCEEDED(dialog->Show(g_hWnd))) result = GetDialogPaths(dialog);
    dialog->Release();
    return result;
}

static std::wstring BrowseForOpenFile(const wchar_t *title, bool archiveOnly) {
    auto paths = BrowseForOpenFiles(title, archiveOnly, false);
    return paths.empty() ? L"" : paths.front();
}

static std::wstring BrowseForFolder(const wchar_t *title) {
    auto paths = BrowseForFolders(title);
    return paths.empty() ? L"" : paths.front();
}

static void AddBackupSources(const std::vector<std::wstring> &paths) {
    for (const auto &path : paths) {
        if (std::find(g_backupSources.begin(), g_backupSources.end(), path) == g_backupSources.end()) {
            g_backupSources.push_back(path);
            LogInfo(L"已添加备份来源: " + path);
        }
    }
    RefreshBackupSources();
}

static std::wstring BrowseForSaveFile(const wchar_t *title) {
    IFileSaveDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return L"";
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    const COMDLG_FILTERSPEC filters[] = {
        {L"ABK 备份文件 (*.abk)", L"*.abk"}, {L"所有文件 (*.*)", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetDefaultExtension(L"abk");
    std::wstring result;
    if (SUCCEEDED(dialog->Show(g_hWnd))) result = GetDialogPath(dialog);
    dialog->Release();
    return result;
}

// ═══════════════════ 打包 / 解包操作 ═══════════════════

struct PackThreadContext {
    std::vector<std::string> sources;
    std::string dst;
    PackOptions options;
};

struct UnpackThreadContext {
    std::string src;
    std::string dst;
    std::string password;
};

static DWORD WINAPI PackThreadProc(LPVOID raw) {
    PackThreadContext *context = static_cast<PackThreadContext *>(raw);
    std::string error;
    bool ok = Packer::pack(context->sources, context->dst, context->options, error);
    PostMessage(g_hWnd, WM_USER + 1, ok ? 1 : 0,
                ok ? 0 : reinterpret_cast<LPARAM>(new std::string(error)));
    delete context;
    return 0;
}

static DWORD WINAPI UnpackThreadProc(LPVOID raw) {
    UnpackThreadContext *context = static_cast<UnpackThreadContext *>(raw);
    std::string error;
    bool ok = Packer::unpack(context->src, context->dst, error, context->password);
    PostMessage(g_hWnd, WM_USER + 2, ok ? 1 : 0,
                ok ? 0 : reinterpret_cast<LPARAM>(new std::string(error)));
    delete context;
    return 0;
}

static void DoPack() {
    std::vector<std::string> sources;
    for (const auto &source : g_backupSources) sources.push_back(WtoU(source));
    std::string dst = WtoU(GetEditText(g_hBackupDst));
    PackOptions options;
    options.password = WtoU(GetEditText(g_hBackupPassword));
    std::string filterError;
    if (!ParseFilterOptions(WtoU(GetEditText(g_hBackupFilter)), options.filter, filterError)) {
        LogError(L"筛选条件无效: " + UtoW(filterError));
        return;
    }

    EnableWindow(g_hBtnPack, FALSE);
    SetWindowTextW(g_hBtnPack, L"打包中…");
    LogInfo(L"正在打包…");

    // 在后台线程执行打包，避免阻塞 UI
    PackThreadContext *context = new PackThreadContext{sources, dst, options};
    HANDLE thread = CreateThread(nullptr, 0, PackThreadProc, context, 0, nullptr);
    if (thread == nullptr) {
        delete context;
        EnableWindow(g_hBtnPack, TRUE);
        SetWindowTextW(g_hBtnPack, L"开始备份");
        LogError(L"无法创建后台打包线程");
        return;
    }
    CloseHandle(thread);
}

static void DoUnpack() {
    std::string src = WtoU(GetEditText(g_hRestoreSrc));
    std::string dst = WtoU(GetEditText(g_hRestoreDst));
    std::string password = WtoU(GetEditText(g_hRestorePassword));

    EnableWindow(g_hBtnUnpack, FALSE);
    SetWindowTextW(g_hBtnUnpack, L"还原中…");
    LogInfo(L"正在还原…");

    UnpackThreadContext *context = new UnpackThreadContext{src, dst, password};
    HANDLE thread = CreateThread(nullptr, 0, UnpackThreadProc, context, 0, nullptr);
    if (thread == nullptr) {
        delete context;
        EnableWindow(g_hBtnUnpack, TRUE);
        SetWindowTextW(g_hBtnUnpack, L"开始还原");
        LogError(L"无法创建后台还原线程");
        return;
    }
    CloseHandle(thread);
}

// ═══════════════════ 窗口过程 ═══════════════════

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // ── 标题 ──
        CreateWindowW(L"STATIC", L"📦 文件备份工具",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 10, 684, 32, hWnd, nullptr, g_hInst, nullptr);

        // ── Tab 控件 ──
        g_hTab = CreateWindowW(WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | TCS_TABS,
            12, 48, 660, 32, hWnd, (HMENU)ID_TAB, g_hInst, nullptr);

        TCITEMW tie = {};
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPWSTR)L"🗂️ 打包备份";
        TabCtrl_InsertItem(g_hTab, 0, &tie);
        tie.pszText = (LPWSTR)L"📂 还原解包";
        TabCtrl_InsertItem(g_hTab, 1, &tie);

        // ═══ 打包面板 ═══
        int ly1 = 90, ly2 = 160, ly3 = 275;

        g_hBackupSrcLabel = CreateWindowW(L"STATIC", L"备份来源:", WS_CHILD | WS_VISIBLE,
            20, ly1 + 3, 60, 20, hWnd, nullptr, g_hInst, nullptr);
        g_hBackupSrc = CreateWindowW(L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
            85, ly1, 450, 58, hWnd, (HMENU)ID_BACKUP_SRC, g_hInst, nullptr);
        g_hBackupSrcBtn = CreateWindowW(L"BUTTON", L"添加来源...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            545, ly1, 110, 26, hWnd, (HMENU)ID_BACKUP_SRC_BTN, g_hInst, nullptr);
        g_hBackupRemoveBtn = CreateWindowW(L"BUTTON", L"移除所选",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            545, ly1 + 32, 110, 26, hWnd, (HMENU)ID_BACKUP_REMOVE_BTN, g_hInst, nullptr);

        g_hBackupDstLabel = CreateWindowW(L"STATIC", L"备份路径:", WS_CHILD | WS_VISIBLE,
            20, ly2 + 3, 60, 20, hWnd, nullptr, g_hInst, nullptr);
        g_hBackupDst = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_READONLY | WS_BORDER,
            85, ly2, 440, 26, hWnd, (HMENU)ID_BACKUP_DST, g_hInst, nullptr);
        g_hBackupDstBtn = CreateWindowW(L"BUTTON", L"选择路径",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            535, ly2, 120, 26, hWnd, (HMENU)ID_BACKUP_DST_BTN, g_hInst, nullptr);

        g_hBackupPasswordLabel = CreateWindowW(L"STATIC", L"密码(可选):", WS_CHILD | WS_VISIBLE,
            20, 200 + 3, 70, 20, hWnd, nullptr, g_hInst, nullptr);
        g_hBackupPassword = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_PASSWORD | WS_BORDER,
            95, 200, 560, 26, hWnd, (HMENU)ID_BACKUP_PASSWORD, g_hInst, nullptr);

        g_hBackupFilterLabel = CreateWindowW(L"STATIC", L"筛选(可选):", WS_CHILD | WS_VISIBLE,
            20, 240 + 3, 70, 20, hWnd, nullptr, g_hInst, nullptr);
        g_hBackupFilter = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER,
            95, 240, 560, 26, hWnd, (HMENU)ID_BACKUP_FILTER, g_hInst, nullptr);

        g_hBtnPack = CreateWindowW(L"BUTTON", L"开始备份",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            240, ly3, 200, 40, hWnd, (HMENU)ID_BTN_PACK, g_hInst, nullptr);

        // ═══ 还原面板（初始隐藏）═══
        int ry1 = 95, ry2 = 135, ry3 = 250;

        g_hRestoreSrcLabel = CreateWindowW(L"STATIC", L"备份文件:", WS_CHILD,
            20, ry1 + 3, 60, 20, hWnd, nullptr, g_hInst, nullptr);
        g_hRestoreSrc = CreateWindowW(L"EDIT", L"",
            WS_CHILD | ES_READONLY | WS_BORDER,
            85, ry1, 440, 26, hWnd, (HMENU)ID_RESTORE_SRC, g_hInst, nullptr);
        g_hRestoreSrcBtn = CreateWindowW(L"BUTTON", L"选择文件",
            WS_CHILD | BS_PUSHBUTTON,
            535, ry1, 120, 26, hWnd, (HMENU)ID_RESTORE_SRC_BTN, g_hInst, nullptr);

        g_hRestoreDstLabel = CreateWindowW(L"STATIC", L"还原目录:", WS_CHILD,
            20, ry2 + 3, 60, 20, hWnd, nullptr, g_hInst, nullptr);
        g_hRestoreDst = CreateWindowW(L"EDIT", L"",
            WS_CHILD | ES_READONLY | WS_BORDER,
            85, ry2, 440, 26, hWnd, (HMENU)ID_RESTORE_DST, g_hInst, nullptr);
        g_hRestoreDstBtn = CreateWindowW(L"BUTTON", L"选择目录",
            WS_CHILD | BS_PUSHBUTTON,
            535, ry2, 120, 26, hWnd, (HMENU)ID_RESTORE_DST_BTN, g_hInst, nullptr);

        g_hRestorePasswordLabel = CreateWindowW(L"STATIC", L"密码(可选):", WS_CHILD,
            20, 175 + 3, 70, 20, hWnd, nullptr, g_hInst, nullptr);
        g_hRestorePassword = CreateWindowW(L"EDIT", L"",
            WS_CHILD | ES_PASSWORD | WS_BORDER,
            95, 175, 560, 26, hWnd, (HMENU)ID_RESTORE_PASSWORD, g_hInst, nullptr);

        g_hBtnUnpack = CreateWindowW(L"BUTTON", L"开始还原",
            WS_CHILD | BS_PUSHBUTTON,
            240, ry3, 200, 40, hWnd, (HMENU)ID_BTN_UNPACK, g_hInst, nullptr);

        // ═══ 日志区域 ═══
        g_hLog = CreateWindowW(L"EDIT", L"就绪，请选择操作。",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_BORDER | WS_VSCROLL,
            12, 335, 660, 205, hWnd, (HMENU)ID_LOG, g_hInst, nullptr);

        // 设置日志字体为等宽字体
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (hFont) SendMessageW(g_hLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        return 0;
    }

    case WM_SIZE: {
        // 窗口大小改变时不需要特殊处理（固定大小窗口）
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nmhdr = (NMHDR*)lParam;
        if (nmhdr->idFrom == ID_TAB && nmhdr->code == TCN_SELCHANGE) {
            SwitchTab(TabCtrl_GetCurSel(g_hTab));
        }
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        // ── 打包：从一个按钮添加文件或文件夹 ──
        case ID_BACKUP_SRC_BTN: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"添加文件（可多选）");
            AppendMenuW(menu, MF_STRING, 2, L"添加文件夹（可多选）");
            RECT buttonRect = {};
            GetWindowRect(g_hBackupSrcBtn, &buttonRect);
            UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                         buttonRect.left, buttonRect.bottom, 0, hWnd, nullptr);
            DestroyMenu(menu);
            if (choice == 1) AddBackupSources(BrowseForOpenFiles(L"选择要备份的文件", false, true));
            if (choice == 2) AddBackupSources(BrowseForFolders(L"选择要备份的文件夹"));
            return 0;
        }
        case ID_BACKUP_REMOVE_BTN: {
            LRESULT selected = SendMessageW(g_hBackupSrc, LB_GETCURSEL, 0, 0);
            if (selected != LB_ERR && static_cast<size_t>(selected) < g_backupSources.size()) {
                LogInfo(L"已移除备份来源: " + g_backupSources[static_cast<size_t>(selected)]);
                g_backupSources.erase(g_backupSources.begin() + selected);
                RefreshBackupSources();
            }
            return 0;
        }
        // ── 打包：选择保存路径 ──
        case ID_BACKUP_DST_BTN: {
            std::wstring file = BrowseForSaveFile(L"选择备份文件保存位置");
            if (!file.empty()) {
                SetEditText(g_hBackupDst, file);
                LogInfo(L"已选择备份路径: " + file);
                EnableWindow(g_hBtnPack, CanStartBackup());
            }
            return 0;
        }
        // ── 开始打包 ──
        case ID_BTN_PACK: {
            DoPack();
            return 0;
        }
        // ── 还原：选择备份文件 ──
        case ID_RESTORE_SRC_BTN: {
            std::wstring file = BrowseForOpenFile(L"选择 .abk 备份文件", true);
            if (!file.empty()) {
                SetEditText(g_hRestoreSrc, file);
                LogInfo(L"已选择备份文件: " + file);
                EnableWindow(g_hBtnUnpack, AreBothSet(g_hRestoreSrc, g_hRestoreDst));
            }
            return 0;
        }
        // ── 还原：选择目标目录 ──
        case ID_RESTORE_DST_BTN: {
            std::wstring dir = BrowseForFolder(L"选择还原到的目标文件夹");
            if (!dir.empty()) {
                SetEditText(g_hRestoreDst, dir);
                LogInfo(L"已选择还原目录: " + dir);
                EnableWindow(g_hBtnUnpack, AreBothSet(g_hRestoreSrc, g_hRestoreDst));
            }
            return 0;
        }
        // ── 开始解包 ──
        case ID_BTN_UNPACK: {
            DoUnpack();
            return 0;
        }
        }
        break;
    }

    // ── 打包完成回调 (来自后台线程) ──
    case WM_USER + 1: {
        bool ok = (wParam == 1);
        if (ok) {
            LogSuccess(L"打包完成！备份文件已生成。");
        } else {
            std::string *err = reinterpret_cast<std::string*>(lParam);
            LogError(L"打包失败: " + UtoW(*err));
            delete err;
        }
        EnableWindow(g_hBtnPack, TRUE);
        SetWindowTextW(g_hBtnPack, L"开始备份");
        return 0;
    }

    // ── 解包完成回调 (来自后台线程) ──
    case WM_USER + 2: {
        bool ok = (wParam == 1);
        if (ok) {
            LogSuccess(L"还原完成！所有文件已恢复到目标目录。");
        } else {
            std::string *err = reinterpret_cast<std::string*>(lParam);
            LogError(L"还原失败: " + UtoW(*err));
            delete err;
        }
        EnableWindow(g_hBtnUnpack, TRUE);
        SetWindowTextW(g_hBtnUnpack, L"开始还原");
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ═══════════════════ 入口点 ═══════════════════

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;

    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 初始化通用控件（Tab、按钮等视觉样式）
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    // 注册窗口类
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"BackupToolWnd";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 创建主窗口
    int winW = 700, winH = 610;
    g_hWnd = CreateWindowExW(
        0,
        L"BackupToolWnd",
        L"文件备份工具",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(comResult)) CoUninitialize();
    return (int)msg.wParam;
}
