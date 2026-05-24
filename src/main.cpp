
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <filesystem>
#include <omp.h>
#include <opencv2/opencv.hpp>
#include <sstream>
#include <atomic>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

// ---- Window controls IDs -----
#define IDC_INPUT_PATH    101
#define IDC_OUTPUT_PATH   102
#define IDC_FPS_EDIT      103
#define IDC_BTN_INPUT     104
#define IDC_BTN_OUTPUT    105
#define IDC_BTN_START     106
#define IDC_LOG_BOX       107
#define IDC_PROGRESS      108
#define IDC_CORES_LABEL   109

// ---- Global handles ----
HWND g_hWnd;
HWND g_hInputPath, g_hOutputPath, g_hFpsEdit;
HWND g_hBtnInput, g_hBtnOutput, g_hBtnStart;
HWND g_hLogBox, g_hProgress, g_hCoresLabel;
HFONT g_hFont;

// ---- Shared state for worker thread ----
struct WorkParams {
    std::wstring inputFolder;
    std::wstring outputFolder;
    int          fps;
};


void LogLine(const std::wstring& msg) {
   
    HWND hLog = g_hLogBox;
    wchar_t* buf = new wchar_t[msg.size() + 1];
    wcscpy_s(buf, msg.size() + 1, msg.c_str());
    PostMessage(g_hWnd, WM_APP + 2, (WPARAM)buf, 0);
}


void PostProgress(int total, int done) {
    PostMessage(g_hWnd, WM_APP + 3, (WPARAM)total, (LPARAM)done);
}

// ---- Core extraction logic ----

int ExtractFrames(const std::wstring& videoPath,
                  const std::wstring& saveFolder,
                  int fps)
{
    // Convert wstring paths to UTF-8 for OpenCV
    std::string videoPathA(videoPath.begin(), videoPath.end());
    std::string saveFolderA(saveFolder.begin(), saveFolder.end());

    cv::VideoCapture cap(videoPathA);
    if (!cap.isOpened()) return -1;

    double videoFps = cap.get(cv::CAP_PROP_FPS);
    if (videoFps <= 0) videoFps = 30.0;

    
    int frameInterval = static_cast<int>(videoFps / fps);
    if (frameInterval < 1) frameInterval = 1;

    cv::Mat frame;
    int frameIndex  = 0;   
    int savedCount  = 0;

    while (true) {
        bool ok = cap.read(frame);
        if (!ok || frame.empty()) break;

        if (frameIndex % frameInterval == 0) {
            std::ostringstream oss;
            oss << saveFolderA << "\\frame_"
                << std::setw(6) << std::setfill('0') << savedCount
                << ".jpg";
            cv::imwrite(oss.str(), frame);
            savedCount++;
        }
        frameIndex++;
    }

    cap.release();
    return savedCount;
}

// ---- Worker thread entry point ----
DWORD WINAPI WorkerThread(LPVOID lpParam)
{
    WorkParams* p = reinterpret_cast<WorkParams*>(lpParam);
    std::wstring inputFolder  = p->inputFolder;
    std::wstring outputFolder = p->outputFolder;
    int          fps          = p->fps;
    delete p;

    // Collect video files
    std::vector<std::wstring> videoExtensions = {
        L".mp4", L".avi", L".mov", L".mkv", L".wmv", L".flv", L".webm"
    };

    std::vector<fs::path> videos;
    for (auto& entry : fs::directory_iterator(inputFolder)) {
        if (!entry.is_regular_file()) continue;
        std::wstring ext = entry.path().extension().wstring();
        
        for (auto& c : ext) c = towlower(c);
        for (auto& ve : videoExtensions) {
            if (ext == ve) { videos.push_back(entry.path()); break; }
        }
    }

    if (videos.empty()) {
        LogLine(L"[ERROR] No video files found in the input folder.");
        PostMessage(g_hWnd, WM_APP + 1, 0, 0);
        return 0;
    }

    int total = static_cast<int>(videos.size());
    int cores = omp_get_max_threads();

    std::wostringstream wss;
    wss << L"Found " << total << L" video(s). Using up to "
        << cores << L" core(s) in parallel.";
    LogLine(wss.str());

    std::atomic<int> doneCount(0);
    PostProgress(total, 0);

    // ---- OpenMP parallel loop ----

    #pragma omp parallel for schedule(dynamic, 1) num_threads(cores)
    for (int i = 0; i < total; i++)
    {
        const fs::path& vpath = videos[i];
        std::wstring stem    = vpath.stem().wstring();
        std::wstring outDir  = outputFolder + L"\\" + stem;

        // Create output subfolder for this video
        CreateDirectoryW(outDir.c_str(), nullptr);

        int threadId = omp_get_thread_num();

        {
            std::wostringstream msg;
            msg << L"[Core " << threadId << L"] Processing: " << vpath.filename().wstring();
            LogLine(msg.str());
        }

        int saved = ExtractFrames(vpath.wstring(), outDir, fps);

        {
            std::wostringstream msg;
            if (saved >= 0)
                msg << L"[Core " << threadId << L"] Done: "
                    << vpath.filename().wstring()
                    << L"->" << saved << L" frames saved.";
            else
                msg << L"[Core " << threadId << L"] FAILED to open: "
                    << vpath.filename().wstring();
            LogLine(msg.str());
        }

        int done = ++doneCount;
        PostProgress(total, done);
    }
    

    LogLine(L"-------------------------------------------------");
    LogLine(L"All videos processed. Done!");
    PostMessage(g_hWnd, WM_APP + 1, 0, 0); // signal completion
    return 0;
}

// ---- Browse for folder helper ----
std::wstring BrowseFolder(HWND owner, const wchar_t* title)
{
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner  = owner;
    bi.lpszTitle  = title;
    bi.ulFlags    = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
    }
    return std::wstring(path);
}

// ---- Win32 Window Procedure ----
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // ---- Custom messages from worker thread ----
    case WM_APP + 1: 
        EnableWindow(g_hBtnStart, TRUE);
        LogLine(L"-------------------------------------------------");
        break;

    case WM_APP + 2: { // Append log line
        wchar_t* buf = reinterpret_cast<wchar_t*>(wParam);
        SendMessageW(g_hLogBox, LB_ADDSTRING, 0, (LPARAM)buf);
        
        int cnt = (int)SendMessageW(g_hLogBox, LB_GETCOUNT, 0, 0);
        SendMessageW(g_hLogBox, LB_SETTOPINDEX, cnt - 1, 0);
        delete[] buf;
        break;
    }

    case WM_APP + 3: { 
        int total = (int)wParam;
        int done  = (int)lParam;
        if (total > 0) {
            SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, total));
            SendMessageW(g_hProgress, PBM_SETPOS, done, 0);
        }
        break;
    }

    // ---- Button clicks ----
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_BTN_INPUT: {
            std::wstring folder = BrowseFolder(hWnd, L"Select Input Folder (Videos)");
            if (!folder.empty())
                SetWindowTextW(g_hInputPath, folder.c_str());
            break;
        }
        case IDC_BTN_OUTPUT: {
            std::wstring folder = BrowseFolder(hWnd, L"Select Output Folder");
            if (!folder.empty())
                SetWindowTextW(g_hOutputPath, folder.c_str());
            break;
        }
        case IDC_BTN_START: {
            
            wchar_t inBuf[MAX_PATH], outBuf[MAX_PATH], fpsBuf[16];
            GetWindowTextW(g_hInputPath,  inBuf,  MAX_PATH);
            GetWindowTextW(g_hOutputPath, outBuf, MAX_PATH);
            GetWindowTextW(g_hFpsEdit,    fpsBuf, 16);

            if (wcslen(inBuf) == 0 || wcslen(outBuf) == 0) {
                MessageBoxW(hWnd, L"Please select both input and output folders.",
                            L"Missing Input", MB_ICONWARNING);
                break;
            }

            int fps = _wtoi(fpsBuf);
            if (fps <= 0) {
                MessageBoxW(hWnd, L"Please enter a valid FPS (e.g. 1, 2, 5).",
                            L"Invalid FPS", MB_ICONWARNING);
                break;
            }

            
            SendMessageW(g_hLogBox, LB_RESETCONTENT, 0, 0);
            SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
            EnableWindow(g_hBtnStart, FALSE);

            WorkParams* wp = new WorkParams();
            wp->inputFolder  = inBuf;
            wp->outputFolder = outBuf;
            wp->fps          = fps;

            CreateThread(nullptr, 0, WorkerThread, wp, 0, nullptr);
            break;
        }
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// -------- Create GUI controls ----------
void CreateControls(HWND hWnd)
{
    g_hFont = CreateFontW(
        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    auto MakeLabel = [&](const wchar_t* text, int x, int y, int w, int h) {
        HWND hwndLabel = CreateWindowW(L"STATIC", text,
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            x, y, w, h, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hwndLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return hwndLabel;
        };
    auto MakeEdit = [&](int id, int x, int y, int w, int h, bool readOnly = false) {
        DWORD style = WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL;
        if (readOnly) style |= ES_READONLY;

        HWND hwndEdit = CreateWindowW(L"EDIT", L"",
            style, x, y, w, h, hWnd, (HMENU)(INT_PTR)id, nullptr, nullptr);

        SendMessageW(hwndEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return hwndEdit;
        };
    auto MakeButton = [&](const wchar_t* text,
        int id,
        int x,
        int y,
        int w,
        int height)
        {
            HWND hwndButton = CreateWindowW(
                L"BUTTON",
                text,
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                x, y, w, height,
                hWnd,
                (HMENU)(INT_PTR)id,
                nullptr,
                nullptr
            );

            SendMessageW(hwndButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            return hwndButton;
        };

    int margin = 16;
    int y = margin;

   
    // ----- Input folder -----
    MakeLabel(L"Input Folder (Videos):", margin, y + 3, 160, 20);
    g_hInputPath = MakeEdit(IDC_INPUT_PATH, margin + 165, y, 310, 24, true);
    g_hBtnInput  = MakeButton(L"Browse...", IDC_BTN_INPUT, margin + 485, y, 90, 24);
    y += 36;

    // ---- Output folder -----
    MakeLabel(L"Output Folder:", margin, y + 3, 160, 20);
    g_hOutputPath = MakeEdit(IDC_OUTPUT_PATH, margin + 165, y, 310, 24, true);
    g_hBtnOutput  = MakeButton(L"Browse...", IDC_BTN_OUTPUT, margin + 485, y, 90, 24);
    y += 36;

    // ---- FPS ----
    MakeLabel(L"Frames per Second:", margin, y + 3, 160, 20);
    g_hFpsEdit = MakeEdit(IDC_FPS_EDIT, margin + 165, y, 60, 24);
    SetWindowTextW(g_hFpsEdit, L"1");

    // Cores info
    int cores = omp_get_max_threads();
    std::wstring coresTxt = L"CPU Cores available: " + std::to_wstring(cores);
    g_hCoresLabel = MakeLabel(coresTxt.c_str(), margin + 240, y + 3, 250, 20);
    y += 36;

    // ---- Start button -------
    g_hBtnStart = MakeButton(L"Start Extraction", IDC_BTN_START,
                             margin, y, 200, 32);
    y += 44;

    //---Progress bar -------
    g_hProgress = CreateWindowW(PROGRESS_CLASSW, nullptr,
        WS_VISIBLE | WS_CHILD | PBS_SMOOTH,
        margin, y, 584, 18, hWnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
    y += 28;

    //------ Log listbox --------------------------------
    MakeLabel(L"Log:", margin, y, 40, 20);
    y += 20;
    g_hLogBox = CreateWindowW(L"LISTBOX", nullptr,
        WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL |
        LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
        margin, y, 584, 180, hWnd, (HMENU)IDC_LOG_BOX, nullptr, nullptr);
    SendMessageW(g_hLogBox, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Set a monospace font for the log
    HFONT hMono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    SendMessageW(g_hLogBox, WM_SETFONT, (WPARAM)hMono, TRUE);
}

// ---------- WinMain ------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    CoInitialize(nullptr);

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VFEClass";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        0, L"VFEClass",
        L"Video Frame Extractor with Parallel Computing",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 450,
        nullptr, nullptr, hInstance, nullptr);

    CreateControls(g_hWnd);
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
