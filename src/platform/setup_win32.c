/*
 * setup_win32.c -- the first-run window of a shared build on Windows
 * (recomp_setup.h): choose the disc, then watch the game module being built.
 *
 * The Linux version (setup_sdl.c) draws its own widgets with SDL; here the
 * system's controls do the same job: an edit field for the path (typed,
 * pasted, or a file dropped on the window), the shell's file dialog for
 * Browse, a progress bar and the last lines of the build's output. The build
 * runs with its output on a pipe, in a job object that kills it, and every
 * compiler it started, when Cancel is pressed or this process ends.
 *
 * Strings cross this interface as UTF-8 and are converted at the Win32 calls.
 */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_setup.h"

/* Common Controls 6: the themed progress bar (and its error state). MSVC
 * embeds this in the executable's manifest. */
#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define WIN_W 600
#define WIN_H 380
#define PAD   20

enum {
    ID_EDIT = 100, ID_B0 = 200, ID_B1, ID_B2, ID_B3
};

static struct {
    HWND wnd, heading, body, edit, status, timer, tail, progress, b[4];
    HFONT font, bold, small_font;
    HBRUSH bg;
    int open;
    int clicked;                 /* button index pressed, -1 none */
    int closed;                  /* the window's close box, or Escape */
    int enter;                   /* Return in the window */
    int error_status;            /* status line drawn in the error colour */
    char dropped[4096];
} S = { 0 };

/* ------------------------------------------------------------- strings */

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, n);
    return w;
}

static void narrow(const wchar_t *w, char *out, size_t n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL))
        out[0] = '\0';
}

static void set_text(HWND h, const char *s)
{
    wchar_t *w = wide(s);
    if (h && w) SetWindowTextW(h, w);
    free(w);
}

/* ------------------------------------------------------------- window */

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(wp) >= ID_B0 && LOWORD(wp) <= ID_B3 && HIWORD(wp) == BN_CLICKED) {
            S.clicked = LOWORD(wp) - ID_B0;
            return 0;
        }
        break;
    case WM_CLOSE:
        S.closed = 1;
        return 0;
    case WM_DROPFILES: {
        wchar_t path[2048];
        if (DragQueryFileW((HDROP)wp, 0, path, 2048))
            narrow(path, S.dropped, sizeof S.dropped);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        /* Opaque: a static control's new text is drawn over the old. */
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if ((HWND)lp == S.status && S.error_status)
            SetTextColor(dc, RGB(176, 32, 32));
        else if ((HWND)lp == S.tail || (HWND)lp == S.timer)
            SetTextColor(dc, RGB(96, 96, 96));
        else
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)S.bg;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static HWND control(const wchar_t *cls, DWORD style, int x, int y, int w, int h,
                    int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, L"", WS_CHILD | style, x, y, w, h, S.wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

int recomp_setup_open(const char *window_title)
{
    WNDCLASSW wc;
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    RECT r = { 0, 0, WIN_W, WIN_H };
    wchar_t *title;
    int i, x, y;

    if (S.open)
        return 1;
    InitCommonControls();
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    lf = ncm.lfMessageFont;
    S.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 3 / 2;
    S.bold = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 9 / 10;
    S.small_font = CreateFontIndirectW(&lf);
    S.bg = GetSysColorBrush(COLOR_WINDOW);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    wc.hbrBackground = S.bg;
    wc.lpszClassName = L"xboxrecomp_setup";
    RegisterClassW(&wc);

    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    title = wide(window_title);
    S.wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, title ? title : L"",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2,
                            r.right - r.left, r.bottom - r.top,
                            NULL, NULL, wc.hInstance, NULL);
    free(title);
    if (!S.wnd)
        return 0;

    x = PAD;
    y = PAD;
    S.heading  = control(L"STATIC", SS_LEFT, x, y, WIN_W - 2 * PAD, 30, 0, S.bold);
    S.body     = control(L"STATIC", SS_LEFT, x, y + 40, WIN_W - 2 * PAD, 110, 0, S.font);
    S.edit     = control(L"EDIT", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                         x, y + 160, WIN_W - 2 * PAD, 26, ID_EDIT, S.font);
    S.progress = control(PROGRESS_CLASSW, 0, x, y + 160, WIN_W - 2 * PAD, 16, 0, S.font);
    SendMessageW(S.progress, PBM_SETRANGE32, 0, 1000);
    S.status   = control(L"STATIC", SS_LEFT, x, y + 196, WIN_W - 2 * PAD - 60, 60, 0, S.font);
    S.timer    = control(L"STATIC", SS_RIGHT, WIN_W - PAD - 60, y + 196, 60, 20, 0, S.font);
    /* SS_EDITCONTROL: a long path wraps where it must, not off the edge. */
    S.tail     = control(L"STATIC", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, x, y + 222, WIN_W - 2 * PAD,
                         WIN_H - y - 222 - 60, 0, S.small_font);
    for (i = 0; i < 4; i++)
        S.b[i] = control(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP,
                         WIN_W - PAD - (4 - i) * 112 + 8, WIN_H - PAD - 30, 104, 30,
                         ID_B0 + i, S.font);
    ShowWindow(S.wnd, SW_SHOWNORMAL);
    SetForegroundWindow(S.wnd);
    S.open = 1;
    return 1;
}

void recomp_setup_close(void)
{
    MSG msg;
    if (!S.open)
        return;
    DestroyWindow(S.wnd);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(S.font);
    DeleteObject(S.bold);
    DeleteObject(S.small_font);
    memset(&S, 0, sizeof S);
}

/* Shows the controls a screen uses and labels its buttons (NULL: hidden;
 * buttons sit at the right, the last one rightmost). */
static void screen(const char *heading, const char *body, int edit, int progress,
                   const char *b0, const char *b1, const char *b2, const char *b3)
{
    const char *labels[4] = { b0, b1, b2, b3 };
    int i;
    set_text(S.heading, heading);
    set_text(S.body, body);
    set_text(S.status, "");
    set_text(S.timer, "");
    set_text(S.tail, "");
    S.error_status = 0;
    ShowWindow(S.heading, SW_SHOW);
    ShowWindow(S.body, SW_SHOW);
    ShowWindow(S.status, SW_SHOW);
    ShowWindow(S.timer, SW_SHOW);
    ShowWindow(S.tail, SW_SHOW);
    ShowWindow(S.edit, edit ? SW_SHOW : SW_HIDE);
    ShowWindow(S.progress, progress ? SW_SHOW : SW_HIDE);
    for (i = 0; i < 4; i++) {
        set_text(S.b[i], labels[i] ? labels[i] : "");
        ShowWindow(S.b[i], labels[i] ? SW_SHOW : SW_HIDE);
        EnableWindow(S.b[i], TRUE);
    }
    S.clicked = -1;
    S.closed = 0;
    S.enter = 0;
    S.dropped[0] = '\0';
}

static void status(const char *s, int is_error)
{
    S.error_status = is_error;
    set_text(S.status, s);
    InvalidateRect(S.status, NULL, TRUE);
}

/* One round of the message loop; Return and Escape become flags. */
static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            S.closed = 1;
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            S.enter = 1;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* -------------------------------------------------------------- picking */

static void tidy_path(char *p)
{
    size_t n;
    while (*p == ' ' || *p == '\'' || *p == '"') memmove(p, p + 1, strlen(p));
    n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\'' || p[n - 1] == '"' ||
                 p[n - 1] == '\n' || p[n - 1] == '\r'))
        p[--n] = '\0';
}

/* The shell's file dialog: a disc image (or default.xbe), or with `folder` a
 * folder. 1 with the choice in out. */
static int browse(int folder, char *out, size_t n)
{
    IFileOpenDialog *d = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    DWORD opts = 0;
    int ok = 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IFileOpenDialog, (void **)&d))) {
        IFileOpenDialog_GetOptions(d, &opts);
        opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (folder) {
            opts |= FOS_PICKFOLDERS;
            IFileOpenDialog_SetTitle(d, L"The folder the disc was extracted to");
        } else {
            static const COMDLG_FILTERSPEC types[] = {
                { L"Xbox disc image or default.xbe", L"*.iso;*.xiso;default.xbe" },
                { L"All files", L"*.*" },
            };
            opts |= FOS_FILEMUSTEXIST;
            IFileOpenDialog_SetFileTypes(d, 2, types);
            IFileOpenDialog_SetTitle(d, L"Your Xbox disc image");
        }
        IFileOpenDialog_SetOptions(d, opts);
        if (SUCCEEDED(IFileOpenDialog_Show(d, S.wnd)) &&
            SUCCEEDED(IFileOpenDialog_GetResult(d, &item)) &&
            SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            narrow(path, out, n);
            ok = out[0] != '\0';
        }
        if (path) CoTaskMemFree(path);
        if (item) IShellItem_Release(item);
        IFileOpenDialog_Release(d);
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    return ok;
}

static int ask_terminal(const char *heading, const char *body, const char *error,
                        recomp_setup_validate_fn validate, void *ctx,
                        char *out, size_t out_bytes)
{
    char line[4096], msg[1024];
    fprintf(stderr, "\n%s\n\n%s\n", heading, body);
    if (error)
        fprintf(stderr, "\n%s\n", error);
    for (;;) {
        fprintf(stderr, "\nPath (empty to quit): ");
        if (!fgets(line, sizeof line, stdin))
            return 0;
        tidy_path(line);
        if (!line[0])
            return 0;
        if (validate(line, msg, sizeof msg, ctx)) {
            snprintf(out, out_bytes, "%s", line);
            return 1;
        }
        fprintf(stderr, "%s\n", msg);
    }
}

int recomp_setup_pick(const char *heading, const char *body, const char *error,
                      recomp_setup_validate_fn validate, void *ctx,
                      char *out, size_t out_bytes)
{
    char field[4096];
    if (!S.open)
        return ask_terminal(heading, body, error, validate, ctx, out, out_bytes);
    screen(heading, body, 1, 0, "Continue", "Disc image...", "Folder...", "Quit");
    SendMessageW(S.edit, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"Type or paste a path, drop a file here, or browse");
    SetWindowTextW(S.edit, L"");
    SetFocus(S.edit);
    if (error)
        status(error, 1);

    for (;;) {
        int check = 0;
        pump();
        if (S.closed || S.clicked == 3)
            return 0;
        if (S.dropped[0]) {
            set_text(S.edit, S.dropped);
            S.dropped[0] = '\0';
            check = 1;
        }
        if (S.clicked == 1 || S.clicked == 2) {
            char chosen[4096];
            int folder = S.clicked == 2;
            S.clicked = -1;
            if (browse(folder, chosen, sizeof chosen)) {
                set_text(S.edit, chosen);
                check = 1;
            }
        }
        if (S.clicked == 0 || S.enter) {
            S.clicked = -1;
            S.enter = 0;
            check = 1;
        }
        if (check) {
            wchar_t w[4096];
            char msg[1024];
            GetWindowTextW(S.edit, w, 4096);
            narrow(w, field, sizeof field);
            tidy_path(field);
            if (!field[0]) {
                status("Choose the disc image or folder first.", 1);
                continue;
            }
            status("Checking...", 0);
            UpdateWindow(S.wnd);
            if (validate(field, msg, sizeof msg, ctx)) {
                snprintf(out, out_bytes, "%s", field);
                return 1;
            }
            status(msg, 1);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 30, QS_ALLINPUT);
    }
}

/* --------------------------------------------------------------- running */

typedef struct Child {
    HANDLE process, job, pipe;
    char buf[8192];
    size_t len;
    int eof;
    FILE *log;
} Child;

static void mkdir_parents(const char *path)
{
    char tmp[4096], *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++)
        if (*p == '/' || *p == '\\') {
            char c = *p;
            wchar_t *w;
            *p = '\0';
            if ((w = wide(tmp))) {
                CreateDirectoryW(w, NULL);
                free(w);
            }
            *p = c;
        }
}

/* argv as one command line, quoted the way the C runtime splits it again. */
static void append_arg(char *cmd, size_t n, const char *a)
{
    size_t len = strlen(cmd);
    int quote = !a[0] || strpbrk(a, " \t\"") != NULL;
    const char *p;
    if (len && len + 1 < n) cmd[len++] = ' ';
    if (quote && len + 1 < n) cmd[len++] = '"';
    for (p = a; *p && len + 4 < n; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) {                              /* backslashes before the closing quote */
            while (bs-- && len + 3 < n) { cmd[len++] = '\\'; if (quote) cmd[len++] = '\\'; }
            break;
        }
        if (*p == '"') {
            while (bs-- && len + 3 < n) { cmd[len++] = '\\'; cmd[len++] = '\\'; }
            cmd[len++] = '\\';
            cmd[len++] = '"';
        } else {
            while (bs-- && len + 2 < n) cmd[len++] = '\\';
            cmd[len++] = *p;
        }
    }
    if (quote && len + 1 < n) cmd[len++] = '"';
    cmd[len] = '\0';
}

static int child_start(Child *c, char *const argv[], const char *log_path, char *why, size_t wn)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, nul;
    char cmd[16384] = "";
    wchar_t *wcmd;
    int i, ok;

    memset(c, 0, sizeof *c);
    if (log_path) {
        mkdir_parents(log_path);
        c->log = fopen(log_path, "w");
    }
    for (i = 0; argv[i]; i++)
        append_arg(cmd, sizeof cmd, argv[i]);
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(why, wn, "no pipe (error %lu)", GetLastError());
        return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    wcmd = wide(cmd);
    /* Python's output on a pipe is block-buffered; the progress lines must
     * come as they are printed. */
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    ok = wcmd && CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
                                CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    free(wcmd);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        DWORD e = GetLastError();
        CloseHandle(rd);
        snprintf(why, wn, e == ERROR_FILE_NOT_FOUND ? "not found" : "error %lu",
                 (unsigned long)e);
        if (c->log) {
            fprintf(c->log, "could not start %s: %s\n", argv[0], why);
            fclose(c->log);
            c->log = NULL;
        }
        return 0;
    }
    /* Everything it starts is in the job, and the job goes when its last
     * handle does: on Cancel, and when this process exits however it exits. */
    c->job = CreateJobObjectW(NULL, NULL);
    if (c->job) {
        memset(&lim, 0, sizeof lim);
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        AssignProcessToJobObject(c->job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c->process = pi.hProcess;
    c->pipe = rd;
    return 1;
}

/* Complete lines read so far, one per call; NULL when none is waiting (with
 * `block`, waits for one or the end). */
static const char *child_line(Child *c, int block)
{
    static char line[sizeof c->buf];
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        DWORD avail = 0, got = 0;
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            memcpy(line, c->buf, n);
            line[n] = '\0';
            if (n && line[n - 1] == '\r')
                line[n - 1] = '\0';
            memmove(c->buf, nl + 1, c->len - n - 1);
            c->len -= n + 1;
            if (c->log)
                fprintf(c->log, "%s\n", line);
            return line;
        }
        if (c->len == sizeof c->buf) {          /* an overlong line: cut it */
            memcpy(line, c->buf, c->len - 1);
            line[c->len - 1] = '\0';
            c->len = 0;
            return line;
        }
        if (c->eof)
            return NULL;
        if (!block) {
            if (!PeekNamedPipe(c->pipe, NULL, 0, NULL, &avail, NULL)) {
                c->eof = 1;
                continue;
            }
            if (!avail)
                return NULL;
        }
        if (!ReadFile(c->pipe, c->buf + c->len, (DWORD)(sizeof c->buf - c->len), &got, NULL) ||
            got == 0) {
            c->eof = 1;
            if (c->len) {                       /* a last line without a newline */
                c->buf[c->len++] = '\n';
            }
            continue;
        }
        c->len += got;
    }
}

static int child_finish(Child *c, int kill)
{
    DWORD code = 128;
    if (kill && c->job)
        TerminateJobObject(c->job, 1);
    else if (kill)
        TerminateProcess(c->process, 1);
    WaitForSingleObject(c->process, INFINITE);
    GetExitCodeProcess(c->process, &code);
    CloseHandle(c->process);
    CloseHandle(c->pipe);
    if (c->job)
        CloseHandle(c->job);
    if (c->log)
        fclose(c->log);
    memset(c, 0, sizeof *c);
    return (int)code;
}

static int run_terminal(char *const argv[], const char *log_path)
{
    Child c;
    const char *line;
    char why[128];
    if (!child_start(&c, argv, log_path, why, sizeof why)) {
        fprintf(stderr, "could not start %s: %s\n", argv[0], why);
        return 0;
    }
    while ((line = child_line(&c, 1)) != NULL) {
        if (strncmp(line, "@progress ", 10) == 0) {
            int lo, hi, off = 0;
            if (sscanf(line + 10, "%d %d %n", &lo, &hi, &off) >= 2)
                fprintf(stderr, "[%3d%%] %s\n", lo, line + 10 + off);
        } else {
            fprintf(stderr, "%s\n", line);
        }
    }
    return child_finish(&c, 0) == 0;
}

int recomp_setup_run(const char *heading, const char *body, char *const argv[],
                     const char *log_path)
{
    if (!S.open)
        return run_terminal(argv, log_path);
    for (;;) {
        Child c;
        int started, failed = 0, retry = 0;
        float lo = 0, hi = 2, shown = 0;
        DWORD t0 = GetTickCount(), stage_t = t0, last_secs = ~0u;
        char what[256] = "Starting", error[1024] = "", tail[6][256], why[128];
        int tail_n = 0;

        memset(tail, 0, sizeof tail);
        screen(heading, body, 0, 1, NULL, NULL, NULL, "Cancel");
        SendMessageW(S.progress, PBM_SETSTATE, PBST_NORMAL, 0);
        SendMessageW(S.progress, PBM_SETPOS, 0, 0);
        started = child_start(&c, argv, log_path, why, sizeof why);
        if (!started) {
            snprintf(error, sizeof error, "Could not start %s (%s). It needs to be "
                     "installed for the first-time build.", argv[0], why);
            failed = 1;
        }

        while (!retry) {
            const char *line;
            DWORD now = GetTickCount();
            int tail_changed = 0;

            while (started && !failed && (line = child_line(&c, 0)) != NULL) {
                if (strncmp(line, "@progress ", 10) == 0) {
                    int a, b2, off = 0;
                    if (sscanf(line + 10, "%d %d %n", &a, &b2, &off) >= 2) {
                        lo = (float)a;
                        hi = (float)b2;
                        if (shown < lo) shown = lo;
                        stage_t = now;
                        snprintf(what, sizeof what, "%s", line + 10 + off);
                        status(what, 0);
                    }
                } else if (line[0]) {
                    if (strncmp(line, "error: ", 7) == 0)
                        snprintf(error, sizeof error, "%s", line + 7);
                    if (tail_n == 6) {
                        memmove(tail[0], tail[1], sizeof tail[0] * 5);
                        tail_n = 5;
                    }
                    snprintf(tail[tail_n++], sizeof tail[0], "%s", line);
                    tail_changed = 1;
                }
            }
            if (started && !failed && c.eof && c.len == 0) {
                int rc = child_finish(&c, 0);
                started = 0;
                if (rc == 0)
                    return 1;
                failed = 1;
                if (!error[0])
                    snprintf(error, sizeof error, "The build stopped (exit status %d).", rc);
            }
            if (failed && IsWindowVisible(S.b[3]) && S.clicked != 0) {
                /* Switch to the failure screen once. */
                char shown_err[1400];
                if (!IsWindowVisible(S.b[0])) {
                    screen(heading, body, 0, 1, "Try again", "Open log", NULL, "Quit");
                    SendMessageW(S.progress, PBM_SETSTATE, PBST_ERROR, 0);
                    SendMessageW(S.progress, PBM_SETPOS, (WPARAM)(shown * 10), 0);
                    snprintf(shown_err, sizeof shown_err, "%s%s%s", error,
                             log_path ? "\nFull log: " : "", log_path ? log_path : "");
                    status(shown_err, 1);
                }
            }

            pump();
            if (S.closed || S.clicked == 3) {
                fprintf(stderr, "[SETUP] build stopped: %s\n",
                        S.closed ? "the window was closed" : failed ? "Quit" : "Cancel");
                if (started)
                    child_finish(&c, 1);
                return 0;
            }
            if (failed && (S.clicked == 0 || S.enter)) {
                retry = 1;
                continue;
            }
            if (failed && S.clicked == 1 && log_path) {
                wchar_t *w = wide(log_path);
                if (w) ShellExecuteW(S.wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
                free(w);
            }
            S.clicked = -1;
            S.enter = 0;

            if (!failed) {
                /* The bar eases from the step's start towards its end, never
                 * quite getting there until the next step says so. */
                float span = hi - lo, tau = span * 2.2f + 1.0f;
                float t = (now - stage_t) / 1000.0f;
                float target = lo + span * (1.0f - expf(-t / tau));
                unsigned secs = (now - t0) / 1000;
                if (target > shown) shown = target;
                SendMessageW(S.progress, PBM_SETPOS, (WPARAM)(shown * 10), 0);
                if (secs != last_secs) {
                    char tm[32];
                    snprintf(tm, sizeof tm, "%u:%02u", secs / 60, secs % 60);
                    set_text(S.timer, tm);
                    last_secs = secs;
                }
                if (tail_changed) {
                    char all[6 * 260] = "";
                    int i;
                    for (i = 0; i < tail_n; i++) {
                        strncat(all, tail[i], sizeof all - strlen(all) - 3);
                        strncat(all, "\r\n", sizeof all - strlen(all) - 1);
                    }
                    set_text(S.tail, all);
                }
            }
            MsgWaitForMultipleObjects(0, NULL, FALSE, 30, QS_ALLINPUT);
        }
    }
}

int recomp_setup_capture(char *const argv[], char *msg, size_t msg_bytes)
{
    Child c;
    const char *line;
    char last[1024] = "", err[1024] = "", why[128];
    if (!child_start(&c, argv, NULL, why, sizeof why)) {
        snprintf(msg, msg_bytes, "Could not start %s (%s).", argv[0], why);
        return -1;
    }
    while ((line = child_line(&c, 1)) != NULL) {
        if (strncmp(line, "error: ", 7) == 0)
            snprintf(err, sizeof err, "%s", line + 7);
        else if (line[0])
            snprintf(last, sizeof last, "%s", line);
        if (S.open)
            pump();
    }
    snprintf(msg, msg_bytes, "%s", err[0] ? err : last);
    return child_finish(&c, 0);
}

/* --------------------------------------------------------------- message */

void recomp_setup_message(const char *heading, const char *body)
{
    if (!S.open) {
        wchar_t *h = wide(heading), *b = wide(body);
        fprintf(stderr, "\n%s\n\n%s\n", heading, body);
        MessageBoxW(NULL, b ? b : L"", h ? h : L"", MB_OK | MB_ICONINFORMATION);
        free(h);
        free(b);
        return;
    }
    screen(heading, body, 0, 0, NULL, NULL, NULL, "OK");
    while (!S.closed && S.clicked != 3 && !S.enter) {
        pump();
        MsgWaitForMultipleObjects(0, NULL, FALSE, 30, QS_ALLINPUT);
    }
}

#endif /* _WIN32 */
