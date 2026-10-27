/* Setup.exe -- the Panzer Dragoon Orta PC installer.
 *
 * Asks for the player's own copy of the game (a disc image or a folder of its
 * files) and an install folder, downloads a private Python next to itself,
 * then runs installer/setup_main.py, which fetches the compiler, translates
 * the game and builds it. Progress and the log come back over a pipe as
 * lines; "@@PROGRESS n text", "@@ERROR text" and "@@DONE exe" are ours.
 *
 * Built without a C runtime (Win32 only), so it runs on any Windows 10/11
 * machine as it is. Build: see installer/build_setup.sh. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <urlmon.h>
#include "unzip.h"

#define PY_VER   L"3.13.2"
#define PY_URL   L"https://www.python.org/ftp/python/" PY_VER L"/python-" PY_VER L"-embed-amd64.zip"

/* ── tiny runtime ───────────────────────────────────────────────────────── */
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

static void *alloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void release(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static WCHAR *wdup(const WCHAR *s)
{
    int n = lstrlenW(s) + 1;
    WCHAR *d = alloc(n * sizeof(WCHAR));
    if (d) lstrcpyW(d, s);
    return d;
}

static int starts(const WCHAR *s, const WCHAR *p)
{
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

static int to_int(const WCHAR *s)
{
    int v = 0;
    while (*s >= L'0' && *s <= L'9') v = v * 10 + (*s++ - L'0');
    return v;
}

/* ── window ─────────────────────────────────────────────────────────────── */
enum {
    ID_SRC = 100, ID_ISO, ID_FOLDER, ID_DST, ID_DSTB, ID_SHORTCUT, ID_GO,
    ID_PROG, ID_STATUS, ID_LOG, ID_PLAY, ID_OPEN
};
#define WM_LOGLINE  (WM_APP + 1)
#define WM_PROGRESS (WM_APP + 2)
#define WM_FINISHED (WM_APP + 3)

static HWND g_wnd, g_src, g_dst, g_shortcut, g_go, g_prog, g_status, g_log, g_play, g_open;
static HWND g_isob, g_folderb, g_dstb;
static HFONT g_font, g_bold;
static WCHAR g_root[MAX_PATH];            /* the release's files (next to Setup.exe, or unpacked) */
static WCHAR g_self[MAX_PATH];            /* Setup.exe itself */
static BOOL g_payload;                    /* files come from inside Setup.exe */
static BOOL g_auto;                       /* /auto: find the game, install, exit (testing) */
static int g_scanning;
static WCHAR g_exe[MAX_PATH];             /* the built game, once done */
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void post_text(UINT msg, WPARAM w, const WCHAR *text)
{
    PostMessageW(g_wnd, msg, w, (LPARAM)wdup(text ? text : L""));
}

static void log_line(const WCHAR *s) { post_text(WM_LOGLINE, 0, s); }

static void append_log(const WCHAR *s)
{
    int n = GetWindowTextLengthW(g_log);
    if (n > 200000) {                      /* keep the box from growing for ever */
        SendMessageW(g_log, EM_SETSEL, 0, 100000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
        n = GetWindowTextLengthW(g_log);
    }
    SendMessageW(g_log, EM_SETSEL, n, n);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void enable_inputs(BOOL on)
{
    EnableWindow(g_src, on); EnableWindow(g_dst, on); EnableWindow(g_isob, on);
    EnableWindow(g_folderb, on); EnableWindow(g_dstb, on); EnableWindow(g_shortcut, on);
    EnableWindow(g_go, on);
}

/* ── worker ─────────────────────────────────────────────────────────────── */
static WCHAR g_arg_src[MAX_PATH], g_arg_dst[MAX_PATH];
static BOOL g_arg_shortcut;

static BOOL run_wait(WCHAR *cmd, const WCHAR *dir)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi; DWORD code = 1;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

static BOOL file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ── recognising the game ──────────────────────────────────────────────────
 * An Xbox disc image is an XDVDFS volume ("MICROSOFT*XBOX*MEDIA" at sector 32
 * of the game partition). A full disc dump has the video partition first, so
 * the game partition starts further in; an extracted .xiso starts at 0.
 * Inside it, default.xbe's certificate holds the title ID and region. */
#define ORTA_TITLE_ID 0x53450007u
#define REGION_NA     0x00000001u
enum { GAME_BAD, GAME_OTHER, GAME_WRONG_REGION, GAME_ORTA };

static BOOL read_at(HANDLE h, ULONGLONG off, void *buf, DWORD n)
{
    LARGE_INTEGER li; DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, buf, n, &got, NULL) && got == n;
}

static DWORD rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24; }
static WORD  rd16(const unsigned char *p) { return (WORD)(p[0] | p[1] << 8); }

static int check_xbe(HANDLE h, ULONGLONG off, WCHAR *title, int tn)
{
    unsigned char hdr[0x120], cert[0xA4];
    DWORD base, ca;
    int i;
    if (!read_at(h, off, hdr, sizeof hdr) || hdr[0] != 'X' || hdr[1] != 'B' || hdr[2] != 'E' || hdr[3] != 'H')
        return GAME_BAD;
    base = rd32(hdr + 0x104); ca = rd32(hdr + 0x118);
    if (ca < base || ca - base > 0x1000000 || !read_at(h, off + (ca - base), cert, sizeof cert))
        return GAME_BAD;
    if (title) {
        for (i = 0; i < 40 && i < tn - 1; i++) {
            title[i] = (WCHAR)rd16(cert + 0xC + i * 2);
            if (!title[i]) break;
        }
        title[i < tn ? i : tn - 1] = 0;
    }
    if (rd32(cert + 8) != ORTA_TITLE_ID) return GAME_OTHER;
    if (!(rd32(cert + 0xA0) & REGION_NA)) return GAME_WRONG_REGION;
    return GAME_ORTA;
}

static int check_iso(HANDLE h, WCHAR *title, int tn)
{
    static const ULONGLONG bases[] = { 0, 0x18300000ull, 0x0FD90000ull, 0xFD90ull, 0x30600ull, 0x02080000ull, 0x89D80000ull };
    static const char magic[] = "MICROSOFT*XBOX*MEDIA";
    unsigned char vd[0x20];
    int b, i;
    for (b = 0; b < (int)(sizeof bases / sizeof bases[0]); b++) {
        ULONGLONG base = bases[b];
        DWORD root, rsize;
        unsigned char *dir;
        int result = GAME_BAD;
        if (!read_at(h, base + 0x10000, vd, sizeof vd)) continue;
        for (i = 0; i < 20 && vd[i] == (unsigned char)magic[i]; i++) ;
        if (i < 20) continue;
        root = rd32(vd + 0x14); rsize = rd32(vd + 0x18);
        if (!rsize || rsize > (1 << 20)) return GAME_BAD;
        dir = alloc(rsize);
        if (!dir || !read_at(h, base + (ULONGLONG)root * 2048, dir, rsize)) {
            release(dir); return GAME_BAD;
        }
        /* Entries sit back to back in each sector (4-byte aligned) until
         * 0xFFFF padding, so a linear walk sees them all (as tools/xiso does). */
        {
            DWORD sec, off;
            for (sec = 0; sec < rsize && result == GAME_BAD; sec += 2048) {
                DWORD end = sec + 2048 < rsize ? sec + 2048 : rsize;
                for (off = sec; off + 14 <= end; ) {
                    const unsigned char *e = dir + off;
                    int nl = e[13];
                    if ((rd16(e) == 0xFFFF && rd16(e + 2) == 0xFFFF) || nl == 0 || off + 14 + nl > end) break;
                    if (nl == 11) {
                        static const char want[] = "DEFAULT.XBE";
                        for (i = 0; i < 11; i++) {
                            char c = (char)e[14 + i];
                            if (c >= 'a' && c <= 'z') c -= 32;
                            if (c != want[i]) break;
                        }
                        if (i == 11) {
                            result = check_xbe(h, base + (ULONGLONG)rd32(e + 4) * 2048, title, tn);
                            break;
                        }
                    }
                    off += (14 + nl + 3) & ~3u;
                }
            }
        }
        release(dir);
        return result;
    }
    return GAME_BAD;
}

/* A disc image file, or a folder holding default.xbe. */
static int check_game(const WCHAR *path, WCHAR *title, int tn)
{
    DWORD a = GetFileAttributesW(path);
    WCHAR p[MAX_PATH + 16];
    HANDLE h;
    int r;
    if (title && tn) title[0] = 0;
    if (a == INVALID_FILE_ATTRIBUTES) return GAME_BAD;
    if (a & FILE_ATTRIBUTE_DIRECTORY) wsprintfW(p, L"%s\\default.xbe", path);
    else lstrcpynW(p, path, MAX_PATH);
    h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return GAME_BAD;
    r = (a & FILE_ATTRIBUTE_DIRECTORY) ? check_xbe(h, 0, title, tn) : check_iso(h, title, tn);
    CloseHandle(h);
    return r;
}

/* ── looking for the game ───────────────────────────────────────────────── */
#define WM_FOUND (WM_APP + 4)
static volatile LONG g_scan_dirs;

static BOOL ends_ci(const WCHAR *s, const WCHAR *suf)
{
    int a = lstrlenW(s), b = lstrlenW(suf);
    return a >= b && lstrcmpiW(s + a - b, suf) == 0;
}

static BOOL skip_dir(const WCHAR *n)
{
    static const WCHAR *skip[] = { L"Windows", L"Program Files", L"Program Files (x86)",
        L"ProgramData", L"AppData", L"System Volume Information", L"Recovery", L"PerfLogs",
        L"MSOCache", L"node_modules", L"$Recycle.Bin", L"$WinREAgent", L"Windows.old",
        L"_python", L"_llvm-mingw", L"_downloads", L"_obj", NULL };
    int i;
    if (n[0] == L'.' || n[0] == L'$') return TRUE;
    for (i = 0; skip[i]; i++) if (lstrcmpiW(n, skip[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL scan_dir(const WCHAR *dir, int depth, WCHAR *found)
{
    WCHAR pat[MAX_PATH + 4], full[MAX_PATH];
    WIN32_FIND_DATAW fd; HANDLE h; BOOL hit = FALSE;
    if (InterlockedIncrement(&g_scan_dirs) > 40000 || lstrlenW(dir) > MAX_PATH - 70) return FALSE;
    wsprintfW(pat, L"%s\\*", dir);
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 2 /* LARGE_FETCH */);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (fd.cFileName[0] == L'.' || lstrlenW(dir) + lstrlenW(fd.cFileName) + 2 >= MAX_PATH) continue;
        wsprintfW(full, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                !skip_dir(fd.cFileName) && scan_dir(full, depth - 1, found)) { hit = TRUE; break; }
        } else if ((ends_ci(fd.cFileName, L".iso") || ends_ci(fd.cFileName, L".xiso")) &&
                   (fd.nFileSizeHigh || fd.nFileSizeLow > 100u * 1024 * 1024)) {
            if (check_game(full, NULL, 0) == GAME_ORTA) { lstrcpyW(found, full); hit = TRUE; break; }
        } else if (lstrcmpiW(fd.cFileName, L"default.xbe") == 0) {
            if (check_game(dir, NULL, 0) == GAME_ORTA) { lstrcpyW(found, dir); hit = TRUE; break; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return hit;
}

static DWORD WINAPI scan_worker(LPVOID unused)
{
    WCHAR found[MAX_PATH] = L"", p[MAX_PATH], prof[MAX_PATH] = L"";
    static const int csidl[] = { CSIDL_DESKTOPDIRECTORY, CSIDL_PERSONAL, CSIDL_MYVIDEO };
    DWORD drives; int i;
    (void)unused;
    GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    /* Likely places first, shallow; then whole drives a few folders deep. */
    if (prof[0]) {
        wsprintfW(p, L"%s\\Downloads", prof);
        if (scan_dir(p, 3, found)) goto done;
    }
    for (i = 0; i < 3; i++)
        if (SUCCEEDED(SHGetFolderPathW(NULL, csidl[i], NULL, 0, p)) && scan_dir(p, 3, found)) goto done;
    if (prof[0] && scan_dir(prof, 3, found)) goto done;
    drives = GetLogicalDrives();
    for (i = 0; i < 26; i++) {
        WCHAR root[4] = { (WCHAR)(L'A' + i), L':', L'\\', 0 };
        UINT t;
        if (!(drives & (1u << i))) continue;
        t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        root[2] = 0;                                  /* "G:" -- scan_dir adds the slash */
        if (scan_dir(root, 4, found)) goto done;
    }
done:
    PostMessageW(g_wnd, WM_FOUND, 0, (LPARAM)(found[0] ? wdup(found) : NULL));
    return 0;
}

/* The embeddable Python only searches the paths its ._pth file lists: add
 * site-packages (where capstone goes) and the toolkit (the pipeline). */
static BOOL fix_pth(const WCHAR *pydir)
{
    WCHAR pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd; HANDLE h; char *buf; DWORD n = 0, w;
    wsprintfW(pat, L"%s\\python3*._pth", pydir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(h);
    wsprintfW(path, L"%s\\%s", pydir, fd.cFileName);
    {   /* zip name = pth name with .zip: python313._pth -> python313.zip */
        WCHAR zip[64]; int i;
        lstrcpynW(zip, fd.cFileName, 60);
        for (i = 0; zip[i] && zip[i] != L'.'; i++) ;
        lstrcpyW(zip + i, L".zip");
        buf = alloc(4 * MAX_PATH * 3);
        n = WideCharToMultiByte(CP_UTF8, 0, zip, -1, buf, MAX_PATH, NULL, NULL) - 1;
    }
    {
        WCHAR tk[MAX_PATH * 2];
        char *p = buf + n;
        wsprintfW(tk, L"\r\n.\r\nLib\\site-packages\r\n%s\\xboxrecomp-main\r\n", g_root);
        n += WideCharToMultiByte(CP_UTF8, 0, tk, -1, p, MAX_PATH * 6, NULL, NULL) - 1;
    }
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { release(buf); return FALSE; }
    WriteFile(h, buf, n, &w, NULL);
    CloseHandle(h);
    release(buf);
    return TRUE;
}

/* ── unzipping (in-process: Wine/Proton on the Steam Deck has no tar.exe) ── */
typedef struct { const WCHAR *dest; int files; DWORD err; } UnzCtx;

static void *unz_alloc(unsigned long n) { return alloc(n); }
static void unz_release(void *p) { release(p); }

static int unz_put(void *vctx, const char *name, int is_dir, const unsigned char *data, unsigned long size)
{
    UnzCtx *c = vctx;
    WCHAR rel[MAX_PATH], path[MAX_PATH * 2], dir[MAX_PATH * 2];
    HANDLE h; DWORD w = 0; int i;
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, rel, MAX_PATH)) return 0;
    for (i = 0; rel[i]; i++) if (rel[i] == L'/') rel[i] = L'\\';
    if (i && rel[i - 1] == L'\\') rel[i - 1] = 0;
    wsprintfW(path, L"%s\\%s", c->dest, rel);
    if (is_dir) {
        SHCreateDirectoryExW(NULL, path, NULL);
        return 1;
    }
    lstrcpyW(dir, path);
    for (i = lstrlenW(dir); i > 0 && dir[i] != L'\\'; i--) ;
    dir[i] = 0;
    SHCreateDirectoryExW(NULL, dir, NULL);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { c->err = GetLastError(); return 0; }
    if (size && (!WriteFile(h, data, size, &w, NULL) || w != size)) {
        c->err = GetLastError();
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    c->files++;
    return 1;
}

/* Unpacks `size` bytes of `path` starting at `offset` (a zip) into dest. */
static BOOL unzip_file(const WCHAR *path, ULONGLONG offset, ULONGLONG size, const WCHAR *dest)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    unsigned char *buf = NULL;
    UnzCtx c;
    int rc = -1, tries;
    LARGE_INTEGER sz;
    WCHAR msg[MAX_PATH + 128];
    /* A file just downloaded is often still open in the virus scanner for a
     * moment: share everything, and try for a few seconds. */
    for (tries = 0; tries < 20; tries++) {
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        Sleep(500);
    }
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfW(msg, L"Could not open %s (Windows error %lu).", path, GetLastError());
        log_line(msg);
        return FALSE;
    }
    if (!size && GetFileSizeEx(h, &sz)) size = (ULONGLONG)sz.QuadPart - offset;
    if (size && size < 0x7FFFFFFF) buf = alloc((SIZE_T)size);
    if (!buf) {
        wsprintfW(msg, L"Could not read %s: size %lu bytes.", path, (DWORD)size);
        log_line(msg);
    } else if (!read_at(h, offset, buf, (DWORD)size)) {
        wsprintfW(msg, L"Could not read %s (Windows error %lu).", path, GetLastError());
        log_line(msg);
    } else {
        c.dest = dest; c.files = 0; c.err = 0;
        SHCreateDirectoryExW(NULL, dest, NULL);
        rc = unz_extract(buf, (unsigned long)size, unz_put, &c, unz_alloc, unz_release);
        if (rc) {
            WCHAR msg[160];
            wsprintfW(msg, L"Unzip error %d after %d files (Windows error %lu).", rc, c.files, c.err);
            log_line(msg);
        }
    }
    release(buf);
    CloseHandle(h);
    return rc == 0;
}

/* Single-file Setup.exe: the release zip is appended to the exe, followed by
 * its size (8 bytes) and "ORTAPAK1". It is unpacked into a work folder under
 * %LOCALAPPDATA%. */
static ULONGLONG payload_size(void)
{
    HANDLE h = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz; unsigned char t[16]; ULONGLONG n = 0; int i;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 16 && read_at(h, sz.QuadPart - 16, t, 16) &&
        t[8] == 'O' && t[9] == 'R' && t[10] == 'T' && t[11] == 'A' && t[12] == 'P' &&
        t[13] == 'A' && t[14] == 'K' && t[15] == '1') {
        for (i = 7; i >= 0; i--) n = n << 8 | t[i];
        if (n + 16 > (ULONGLONG)sz.QuadPart) n = 0;
    }
    CloseHandle(h);
    return n;
}

static BOOL unpack_payload(void)
{
    ULONGLONG n = payload_size();
    LARGE_INTEGER sz;
    HANDLE h;
    BOOL ok = FALSE;
    post_text(WM_PROGRESS, 1, L"Unpacking Setup's files");
    h = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (n && GetFileSizeEx(h, &sz)) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
            ok = unzip_file(g_self, (ULONGLONG)sz.QuadPart - 16 - n, n, g_root);
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    if (!ok) {
        WCHAR msg[MAX_PATH + 96];
        wsprintfW(msg, L"Could not unpack Setup's files into %s. Check that drive has free "
                       L"space and run Setup again.", g_root);
        log_line(msg);
    }
    return ok;
}

static BOOL ensure_python(WCHAR *pyexe)
{
    WCHAR pydir[MAX_PATH], dl[MAX_PATH], zip[MAX_PATH];
    wsprintfW(pydir, L"%s\\_python", g_root);
    wsprintfW(pyexe, L"%s\\python.exe", pydir);
    if (file_exists(pyexe))
        return fix_pth(pydir);
    post_text(WM_PROGRESS, 0, L"Downloading Python");
    wsprintfW(dl, L"%s\\_downloads", g_root);
    CreateDirectoryW(dl, NULL);
    wsprintfW(zip, L"%s\\python-embed.zip", dl);
    if (!file_exists(zip)) {
        log_line(L"Downloading " PY_URL);
        if (URLDownloadToFileW(NULL, PY_URL, zip, 0, NULL) != S_OK) {
            DeleteFileW(zip);
            log_line(L"Could not download Python. Check the internet connection and try again.");
            return FALSE;
        }
    }
    if (!unzip_file(zip, 0, 0, pydir) || !file_exists(pyexe)) {
        if (!file_exists(pyexe)) log_line(L"python.exe was not among the unpacked files.");
        DeleteFileW(zip);
        log_line(L"Could not unpack Python. Run Setup again.");
        return FALSE;
    }
    return fix_pth(pydir);
}

static void handle_line(const WCHAR *line)
{
    if (starts(line, L"@@PROGRESS ")) {
        const WCHAR *p = line + 11;
        int pct = to_int(p);
        while (*p && *p != L' ') p++;
        if (*p) p++;
        post_text(WM_PROGRESS, (WPARAM)(pct + 1), p);
    } else if (starts(line, L"@@DONE ")) {
        lstrcpynW(g_exe, line + 7, MAX_PATH);
    } else if (starts(line, L"@@ERROR ")) {
        WCHAR msg[1024];
        wsprintfW(msg, L"PROBLEM: %s", line + 8);
        log_line(msg);
        post_text(WM_PROGRESS, 0, line + 8);
    } else {
        log_line(line);
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    WCHAR pyexe[MAX_PATH], cmd[MAX_PATH * 4];
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    DWORD code = 1;
    (void)unused;

    g_exe[0] = 0;
    if (g_payload && !unpack_payload()) { PostMessageW(g_wnd, WM_FINISHED, 0, 0); return 0; }
    if (!ensure_python(pyexe)) { PostMessageW(g_wnd, WM_FINISHED, 0, 0); return 0; }

    wsprintfW(cmd, L"\"%s\" -u \"%s\\installer\\setup_main.py\" --source \"%s\" --out \"%s\"",
              pyexe, g_root, g_arg_src, g_arg_dst);
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) { PostMessageW(g_wnd, WM_FINISHED, 0, 0); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_root, &si, &pi)) {
        log_line(L"Could not start the installer script.");
        CloseHandle(rd); CloseHandle(wr);
        PostMessageW(g_wnd, WM_FINISHED, 0, 0);
        return 0;
    }
    CloseHandle(wr);
    {   /* read lines (UTF-8) */
        static char buf[1 << 16];
        static char line[8192];
        static WCHAR wline[8192];
        int ln = 0;
        DWORD got, i;
        while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
            for (i = 0; i < got; i++) {
                char c = buf[i];
                if (c == '\r') continue;
                if (c == '\n' || ln == (int)sizeof line - 1) {
                    line[ln] = 0;
                    MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 8192);
                    handle_line(wline);
                    ln = 0;
                    if (c == '\n') continue;
                }
                line[ln++] = c;
            }
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code == 0 && g_exe[0]) {
        /* Windows scans a program it has never seen the first time it runs,
         * and every player's build is new: start it once now (it quits at
         * once with --prewarm) so that wait happens here, not at first play. */
        WCHAR pw[MAX_PATH + 32], dir[MAX_PATH];
        DWORD t0 = GetTickCount();
        int i;
        post_text(WM_PROGRESS, 99, L"Preparing the graphics shaders (a few minutes; this makes the first start quick)");
        log_line(L"Preparing the graphics shaders, so the first start is quick...");
        lstrcpyW(dir, g_exe);
        for (i = lstrlenW(dir); i > 0 && dir[i] != L'\\'; i--) ;
        dir[i] = 0;
        wsprintfW(pw, L"\"%s\" --prewarm", g_exe);
        run_wait(pw, dir);
        {
            WCHAR msg[96];
            wsprintfW(msg, L"Shaders ready in %lu seconds.", (GetTickCount() - t0 + 500) / 1000);
            log_line(msg);
        }
    }
    PostMessageW(g_wnd, WM_FINISHED, code == 0 && g_exe[0], 0);
    return 0;
}

/* ── shortcut ───────────────────────────────────────────────────────────── */
static const GUID kCLSID_ShellLink = {0x00021401, 0, 0, {0xC0,0,0,0,0,0,0,0x46}};
static const GUID kIID_IShellLinkW = {0x000214F9, 0, 0, {0xC0,0,0,0,0,0,0,0x46}};
static const GUID kIID_IPersistFile = {0x0000010b, 0, 0, {0xC0,0,0,0,0,0,0,0x46}};

static void make_shortcut(const WCHAR *exe)
{
    IShellLinkW *sl = NULL; IPersistFile *pf = NULL;
    WCHAR desk[MAX_PATH], lnk[MAX_PATH], dir[MAX_PATH];
    int i;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk))) return;
    wsprintfW(lnk, L"%s\\Panzer Dragoon Orta.lnk", desk);
    lstrcpyW(dir, exe);
    for (i = lstrlenW(dir); i > 0 && dir[i] != L'\\'; i--) ;
    dir[i] = 0;
    CoInitialize(NULL);
    if (SUCCEEDED(CoCreateInstance(&kCLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &kIID_IShellLinkW, (void **)&sl))) {
        sl->lpVtbl->SetPath(sl, exe);
        sl->lpVtbl->SetWorkingDirectory(sl, dir);
        sl->lpVtbl->SetDescription(sl, L"Panzer Dragoon Orta for PC");
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &kIID_IPersistFile, (void **)&pf))) {
            pf->lpVtbl->Save(pf, lnk, TRUE);
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    CoUninitialize();
}

/* ── pickers ────────────────────────────────────────────────────────────── */
static void pick_iso(void)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW of;
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of; of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Xbox disc images (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Choose your Panzer Dragoon Orta disc image";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&of)) SetWindowTextW(g_src, file);
}

static void pick_folder(HWND target, const WCHAR *title)
{
    BROWSEINFOW bi; LPITEMIDLIST id; WCHAR path[MAX_PATH];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_wnd; bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    CoInitialize(NULL);
    id = SHBrowseForFolderW(&bi);
    if (id) {
        if (SHGetPathFromIDListW(id, path)) SetWindowTextW(target, path);
        CoTaskMemFree(id);
    }
    CoUninitialize();
}

/* ── layout ─────────────────────────────────────────────────────────────── */
static HWND mk(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(lstrcmpW(cls, WC_EDITW) == 0 ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void build_ui(void)
{
    WCHAR def[MAX_PATH], prof[MAX_PATH];
    HWND t;
    t = mk(WC_STATICW, L"Panzer Dragoon Orta for PC", 0, 20, 14, 600, 30, 0);
    SendMessageW(t, WM_SETFONT, (WPARAM)g_bold, TRUE);
    mk(WC_STATICW, L"This builds the game for Windows from your own copy of it: an Xbox disc "
                   L"image (.iso) or a folder holding the game's files (with default.xbe). "
                   L"It looks for your copy by itself, downloads Python and a compiler (about 300 MB) "
                   L"and takes 10-30 minutes.",
       0, 20, 46, 600, 56, 0);
    mk(WC_STATICW, L"Your game:", 0, 20, 106, 90, 20, 0);
    g_src = mk(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, 110, 102, 320, 24, ID_SRC);
    g_isob = mk(WC_BUTTONW, L"Disc image...", WS_TABSTOP, 438, 101, 90, 26, ID_ISO);
    g_folderb = mk(WC_BUTTONW, L"Folder...", WS_TABSTOP, 532, 101, 88, 26, ID_FOLDER);
    mk(WC_STATICW, L"Install to:", 0, 20, 142, 90, 20, 0);
    if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH))
        wsprintfW(def, L"%s\\Games\\Panzer Dragoon Orta PC", prof);
    else
        lstrcpyW(def, L"C:\\Games\\Panzer Dragoon Orta PC");
    g_dst = mk(WC_EDITW, def, ES_AUTOHSCROLL | WS_TABSTOP, 110, 138, 418, 24, ID_DST);
    g_dstb = mk(WC_BUTTONW, L"Browse...", WS_TABSTOP, 532, 137, 88, 26, ID_DSTB);
    g_shortcut = mk(WC_BUTTONW, L"Put a shortcut on the desktop", BS_AUTOCHECKBOX | WS_TABSTOP,
                    110, 172, 300, 22, ID_SHORTCUT);
    SendMessageW(g_shortcut, BM_SETCHECK, BST_CHECKED, 0);
    g_go = mk(WC_BUTTONW, L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 500, 170, 120, 30, ID_GO);
    g_prog = mk(PROGRESS_CLASSW, L"", 0, 20, 214, 600, 20, ID_PROG);
    SendMessageW(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    g_status = mk(WC_STATICW, L"Ready.", SS_ENDELLIPSIS, 20, 240, 600, 20, ID_STATUS);
    g_log = mk(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
               20, 266, 600, 210, ID_LOG);
    g_play = mk(WC_BUTTONW, L"Play", WS_TABSTOP, 380, 486, 116, 30, ID_PLAY);
    g_open = mk(WC_BUTTONW, L"Open folder", WS_TABSTOP, 504, 486, 116, 30, ID_OPEN);
    ShowWindow(g_play, SW_HIDE);
    ShowWindow(g_open, SW_HIDE);
}

/* Keep the log next to the game, for when someone asks for it. */
static void save_log(void)
{
    WCHAR path[MAX_PATH + 32], *text; char *u8; int n, m; DWORD wr; HANDLE h;
    SHCreateDirectoryExW(NULL, g_arg_dst, NULL);
    wsprintfW(path, L"%s\\setup_log.txt", g_arg_dst);
    n = GetWindowTextLengthW(g_log);
    text = alloc((n + 1) * sizeof(WCHAR));
    if (!text) return;
    GetWindowTextW(g_log, text, n + 1);
    m = WideCharToMultiByte(CP_UTF8, 0, text, n, NULL, 0, NULL, NULL);
    u8 = alloc(m + 1);
    if (u8) {
        WideCharToMultiByte(CP_UTF8, 0, text, n, u8, m, NULL, NULL);
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { WriteFile(h, u8, m, &wr, NULL); CloseHandle(h); }
        release(u8);
    }
    release(text);
}

static void start_install(void)
{
    GetWindowTextW(g_src, g_arg_src, MAX_PATH);
    GetWindowTextW(g_dst, g_arg_dst, MAX_PATH);
    g_arg_shortcut = SendMessageW(g_shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!g_arg_src[0] || GetFileAttributesW(g_arg_src) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_wnd, L"Choose your game first: the disc image file, or the folder "
                    L"with the game's files in it.", L"Setup", MB_ICONINFORMATION);
        return;
    }
    {
        WCHAR title[64], msg[512];
        switch (check_game(g_arg_src, title, 64)) {
        case GAME_ORTA: break;
        case GAME_OTHER:
            wsprintfW(msg, L"That is %s, not Panzer Dragoon Orta.\n\nChoose your Panzer Dragoon Orta "
                      L"disc image or game folder.", title[0] ? title : L"a different game");
            MessageBoxW(g_wnd, msg, L"Setup", MB_ICONINFORMATION);
            return;
        case GAME_WRONG_REGION:
            MessageBoxW(g_wnd, L"That is Panzer Dragoon Orta, but not the North American release. "
                        L"Only the North American release is supported for now.", L"Setup", MB_ICONINFORMATION);
            return;
        default:
            MessageBoxW(g_wnd, L"Setup could not read an Xbox game from that. Choose an Xbox disc image "
                        L"(.iso), or the folder that holds the game's files (with default.xbe in it).",
                        L"Setup", MB_ICONINFORMATION);
            return;
        }
    }
    if (!g_arg_dst[0]) {
        MessageBoxW(g_wnd, L"Choose where to install the game.", L"Setup", MB_ICONINFORMATION);
        return;
    }
    enable_inputs(FALSE);
    ShowWindow(g_play, SW_HIDE); ShowWindow(g_open, SW_HIDE);
    SendMessageW(g_prog, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_status, L"Starting...");
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_ISO: pick_iso(); break;
        case ID_FOLDER: pick_folder(g_src, L"Choose the folder with the game's files (default.xbe)"); break;
        case ID_DSTB: pick_folder(g_dst, L"Choose where to install the game"); break;
        case ID_GO: start_install(); break;
        case ID_PLAY:
            if (g_exe[0]) {
                WCHAR dir[MAX_PATH]; int i;
                lstrcpyW(dir, g_exe);
                for (i = lstrlenW(dir); i > 0 && dir[i] != L'\\'; i--) ;
                dir[i] = 0;
                ShellExecuteW(h, L"open", g_exe, NULL, dir, SW_SHOWNORMAL);
            }
            break;
        case ID_OPEN:
            ShellExecuteW(h, L"open", g_arg_dst, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        return 0;
    case WM_LOGLINE:
        append_log((WCHAR *)l);
        release((void *)l);
        return 0;
    case WM_PROGRESS:
        if (w) SendMessageW(g_prog, PBM_SETPOS, (WPARAM)(w - 1), 0);
        SetWindowTextW(g_status, (WCHAR *)l);
        release((void *)l);
        return 0;
    case WM_FOUND:
        g_scanning = 0;
        if (l && GetWindowTextLengthW(g_src) == 0) {
            WCHAR msg[MAX_PATH + 40];
            SetWindowTextW(g_src, (WCHAR *)l);
            wsprintfW(msg, L"Found your game: %s", (WCHAR *)l);
            SetWindowTextW(g_status, msg);
            append_log(msg);
        } else if (!l) {
            SetWindowTextW(g_status, L"Setup did not find your game by itself. Choose it with "
                                     L"\"Disc image...\" or \"Folder...\".");
        } else {
            SetWindowTextW(g_status, L"Ready.");
        }
        if (g_auto) {
            WCHAR d[MAX_PATH];
            if (GetEnvironmentVariableW(L"ORTA_SETUP_DEST", d, MAX_PATH)) SetWindowTextW(g_dst, d);
            SendMessageW(g_shortcut, BM_SETCHECK, BST_UNCHECKED, 0);
            if (l) start_install();
            else ExitProcess(2);
        }
        release((void *)l);
        return 0;
    case WM_FINISHED:
        enable_inputs(TRUE);
        save_log();
        if (g_auto) ExitProcess(w ? 0 : 1);
        if (w) {
            SendMessageW(g_prog, PBM_SETPOS, 100, 0);
            SetWindowTextW(g_status, L"Finished. The game is ready to play.");
            append_log(L"Finished.");
            if (g_arg_shortcut) make_shortcut(g_exe);
            if (g_payload) {     /* the work folder (compiler, Python, sources) is no longer needed */
                WCHAR cmd[MAX_PATH * 2], sys[MAX_PATH];
                STARTUPINFOW si; PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof si); si.cb = sizeof si;
                GetSystemDirectoryW(sys, MAX_PATH);
                wsprintfW(cmd, L"\"%s\\cmd.exe\" /c rmdir /s /q \"%s\"", sys, g_root);
                if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) {
                    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                }
            }
            ShowWindow(g_play, SW_SHOW); ShowWindow(g_open, SW_SHOW);
            SetWindowTextW(g_go, L"Install again");
        } else {
            SetWindowTextW(g_go, L"Try again");
            MessageBoxW(h, L"Setup did not finish. The log in the window says why.\n\n"
                        L"Running it again carries on from where it stopped.",
                        L"Setup", MB_ICONWARNING);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l != g_log) {
            SetBkMode((HDC)w, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void WINAPI start(void)
{
    WNDCLASSW wc; MSG msg; HDC dc; RECT r; int i;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    SetProcessDPIAware();
    InitCommonControlsEx(&icc);
    dc = GetDC(NULL); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc);

    GetModuleFileNameW(NULL, g_self, MAX_PATH);
    lstrcpyW(g_root, g_self);
    for (i = lstrlenW(g_root); i > 0 && g_root[i] != L'\\'; i--) ;
    g_root[i] = 0;
    {
        LPWSTR cl = GetCommandLineW();
        for (; *cl; cl++) if ((cl[0] == L'/' || cl[0] == L'-') && starts(cl + 1, L"auto")) g_auto = TRUE;
    }
    {
        WCHAR check[MAX_PATH], la[MAX_PATH];
        wsprintfW(check, L"%s\\installer\\setup_main.py", g_root);
        if (!file_exists(check) && payload_size()) {
            g_payload = TRUE;
            if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) lstrcpyW(la, g_root);
            wsprintfW(g_root, L"%s\\Orta PC Setup", la);
        } else if (!file_exists(check)) {
            MessageBoxW(NULL, L"Setup.exe needs the rest of the download next to it.\n\n"
                        L"Unzip the whole release into a folder first, then run Setup.exe "
                        L"from there.", L"Panzer Dragoon Orta PC Setup", MB_ICONERROR);
            ExitProcess(1);
        }
    }

    g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_bold = CreateFontW(-S(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, L"Segoe UI");
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"OrtaSetup";
    RegisterClassW(&wc);
    r.left = 0; r.top = 0; r.right = S(640); r.bottom = S(530);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowW(L"OrtaSetup", L"Panzer Dragoon Orta PC - Setup",
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                          NULL, NULL, wc.hInstance, NULL);
    build_ui();
    ShowWindow(g_wnd, SW_SHOW);
    g_scanning = 1;
    SetWindowTextW(g_status, L"Looking for your copy of the game...");
    CloseHandle(CreateThread(NULL, 0, scan_worker, NULL, 0, NULL));
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    ExitProcess(0);
}
