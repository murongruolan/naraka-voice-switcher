// gui.cpp — M3 原生 Win32 GUI · 三步向导 (exe 定位 / 多语言包选择 / 双 Tab 替换)
// 第一步: 选游戏 exe + 语言包目录 (多包时弹窗选择, 已知语言挂中文标签)
// 第二步: 双 Tab — 全量替换 | 按角色替换 (自动备份, 分段进度条)
// 第三步: 恢复原版语音
// 全程宽字符支持中文路径; 失败可 Ctrl+C 复制报错 + gui_error.log
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>
#include "gui.h"
#include "akpk.h"
#include "rebuild.h"
#include "roles.h"

#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <set>
#include <deque>
#include <filesystem>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace nvtool {
namespace {

namespace fs = std::filesystem;

// ---------- 基础工具 ----------
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dir_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
uint64_t file_size_of(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
std::wstring tool_dir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().parent_path().wstring();
}
bool copy_file_progress(const std::wstring& src, const std::wstring& dst,
                        const std::function<void(uint64_t, uint64_t)>& cb) {
    FILE* a = _wfopen(src.c_str(), L"rb");
    if (!a) return false;
    FILE* b = _wfopen(dst.c_str(), L"wb");
    if (!b) { std::fclose(a); return false; }
    uint64_t total = file_size_of(src), done = 0;
    std::vector<char> buf(4 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), a)) > 0) {
        if (std::fwrite(buf.data(), 1, n, b) != n) { std::fclose(a); std::fclose(b); return false; }
        done += n;
        if (cb) cb(done, total ? total : 1);
    }
    std::fclose(a); std::fclose(b);
    return done == total;
}

// ---------- 定位 ----------
const wchar_t* GAME_DATA_DIR = L"NarakaBladepoint_Data";
const wchar_t* GAME_EXE_NAME = L"NarakaBladepoint.exe";
const wchar_t* PCK_NAME = L"ChineseCN.pck";

std::wstring game_windows_dir_from_data(const std::wstring& data_dir) {
    std::wstring w = data_dir + L"\\StreamingAssets\\Audio\\GeneratedSoundBanks\\Windows";
    return file_exists(w + L"\\" + PCK_NAME) ? w : L"";
}
std::wstring find_dir_down(const std::wstring& root, const wchar_t* name, int max_depth) {
    std::deque<std::pair<std::wstring, int>> q;
    q.push_back({root, 0});
    while (!q.empty()) {
        auto cur = q.front(); q.pop_front();
        if (cur.second >= max_depth) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((cur.first + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::wstring sub = cur.first + L"\\" + fd.cFileName;
                if (_wcsicmp(fd.cFileName, name) == 0) { FindClose(h); return sub; }
                q.push_back({sub, cur.second + 1});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return L"";
}
// 从 exe 所在目录(或任意目录)定位游戏的 GeneratedSoundBanks\Windows: 向下找 Data 目录, 失败则向上逐级
std::wstring locate_game_windows(const std::wstring& base) {
    // 向下找 NarakaBladepoint_Data (限深4)
    std::wstring data = find_dir_down(base, GAME_DATA_DIR, 4);
    if (!data.empty()) {
        std::wstring w = game_windows_dir_from_data(data);
        if (!w.empty()) return w;
    }
    // 向上逐级, 且每一级再做一次向下搜索 (处理 exe 在 tool\ 子目录、Data 在 program\ 的包装器布局)
    fs::path p = base;
    for (int i = 0; i < 6 && p.has_parent_path(); i++) {
        p = p.parent_path();
        std::wstring data2 = find_dir_down(p.wstring(), GAME_DATA_DIR, 3);
        if (!data2.empty()) {
            std::wstring w = game_windows_dir_from_data(data2);
            if (!w.empty()) return w;
        }
        std::wstring cand = p.wstring() + L"\\" + GAME_DATA_DIR;
        if (dir_exists(cand)) {
            std::wstring w = game_windows_dir_from_data(cand);
            if (!w.empty()) return w;
        }
    }
    return L"";
}


// 语言参考包: 任意 >100MB 的 .pck, 已知文件名挂中文标签
struct LangRef { std::wstring path, file, label; };
std::wstring label_for_pck(const std::wstring& file) {
    if (_wcsicmp(file.c_str(), L"Japanese.pck") == 0) return L"日语";
    if (_wcsicmp(file.c_str(), L"English.pck") == 0) return L"英语";
    if (_wcsicmp(file.c_str(), L"Thai.pck") == 0) return L"泰语";
    if (_wcsicmp(file.c_str(), L"ChineseCN.pck") == 0) return L"中文(原声)";
    return L"";
}
auto pck_big_enough = [](const std::wstring& p) { return file_exists(p) && file_size_of(p) > 100ull << 20; };

// 在目录里收集全部语言参考包: 顶层 -> Audio\... 子路径 -> 递归限深5
std::vector<LangRef> find_all_lang_pcks(const std::wstring& sel) {
    std::vector<LangRef> out;
    auto add = [&](const std::wstring& full) {
        std::wstring file = fs::path(full).filename().wstring();
        for (auto& r : out) if (_wcsicmp(r.file.c_str(), file.c_str()) == 0) return;  // 按文件名去重
        std::wstring lb = label_for_pck(file);
        out.push_back({full, file, lb});
    };
    auto scan_dir = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.pck").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (sz > 100ull << 20) add(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    scan_dir(sel);
    if (!out.empty()) return out;
    scan_dir(sel + L"\\Audio\\GeneratedSoundBanks\\Windows");
    if (!out.empty()) return out;
    std::deque<std::pair<std::wstring, int>> q;
    q.push_back({sel, 0});
    while (!q.empty()) {
        auto cur = q.front(); q.pop_front();
        if (cur.second >= 5) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((cur.first + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                    q.push_back({cur.first + L"\\" + fd.cFileName, cur.second + 1});
            } else {
                std::wstring f = fd.cFileName;
                if (f.size() > 4 && _wcsicmp(f.c_str() + f.size() - 4, L".pck") == 0 &&
                    (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow) > 100ull << 20)
                    add(cur.first + L"\\" + f);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

// ---------- 全局状态 ----------
enum { PH_IDLE = 0, PH_BACKUP = 1, PH_BUILD = 2, PH_DEPLOY = 3, PH_RESTORE = 4 };
std::atomic<bool> g_busy{false};
std::atomic<int>  g_phase{PH_IDLE};
std::atomic<int>  g_pct{0};
std::atomic<bool> g_running{false};
int  g_anim = 0;

std::wstring g_sel_game;         // 游戏 exe 路径 (兼容旧版: 目录)
std::wstring g_sel_jp;           // 语言包目录
std::wstring g_langref_file;     // 用户选定的参考包文件名 (持久化)
std::wstring g_game_win;         // 解析出的 ...\GeneratedSoundBanks\Windows
std::wstring g_jp_pck;           // 选定的参考 pck 全路径
std::vector<LangRef> g_lang_refs;
bool g_game_ok = false, g_jp_ok = false, g_bak_ok = false;
std::string  g_state = "unknown";
std::set<std::string> g_sel_roles;
bool g_sel_sys = false;
int  g_jp_roles = 0;
int  g_tab = 0;                  // 0=全量替换 1=按角色替换

std::wstring g_op_msg, g_op_detail;
bool g_op_ok = true;

std::wstring pck_path() { return g_game_win + L"\\" + PCK_NAME; }
std::wstring bak_path() { return g_game_win + L"\\" + PCK_NAME + L".bak"; }
std::wstring tmp_out_path() { return fs::path(tool_dir()) / L"build\\gui_tmp_out.pck"; }
std::wstring config_file() { return fs::path(tool_dir()) / L"gui_config.txt"; }
std::wstring error_log_path() { return fs::path(tool_dir()) / L"gui_error.log"; }
std::wstring marker_path() { return pck_path() + L".nvt"; }
std::wstring marker_read() {
    if (!file_exists(marker_path())) return L"";
    FILE* f = _wfopen(marker_path().c_str(), L"rb");
    if (!f) return L"";
    char buf[128] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    return utf8_to_wide(std::string(buf, n));
}
void marker_write(const std::wstring& hash_hex) {
    FILE* f = _wfopen(marker_path().c_str(), L"wb");
    if (!f) return;
    std::string s = wide_to_utf8(hash_hex);
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}
bool reg_read_string(HKEY root, const wchar_t* subkey, const wchar_t* value, std::wstring& out) {
    HKEY k;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[512];
    DWORD sz = sizeof(buf) - sizeof(wchar_t);
    LSTATUS r = RegQueryValueExW(k, value, nullptr, nullptr, (LPBYTE)buf, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return false;
    out.assign(buf, sz / sizeof(wchar_t));
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return true;
}
std::vector<std::wstring> steam_libraries() {
    std::vector<std::wstring> libs;
    std::wstring steam_path;
    if (!reg_read_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steam_path))
        return libs;
    std::wstring vdf = (fs::path(steam_path) / L"steamapps" / L"libraryfolders.vdf").wstring();
    FILE* f = _wfopen(vdf.c_str(), L"rb");
    if (!f) { libs.push_back(steam_path); return libs; }
    std::string s(65536, 0);
    size_t n = std::fread(&s[0], 1, s.size(), f);
    std::fclose(f);
    s.resize(n);
    size_t pos = 0;
    while ((pos = s.find("\"path\"", pos)) != std::string::npos) {
        size_t q1 = s.find('"', pos + 6);
        size_t q2 = s.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        std::string raw = s.substr(q1 + 1, q2 - q1 - 1);
        std::string clean;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') { clean += '\\'; i++; }
            else clean += raw[i];
        }
        libs.push_back(utf8_to_wide(clean));
        pos = q2;
    }
    if (libs.empty()) libs.push_back(steam_path);
    return libs;
}
static bool wcsistr_nocase(const std::wstring& hay, const wchar_t* needle) {
    std::wstring low = hay;
    for (auto& c : low) if (c >= L'A' && c <= L'Z') c += 32;
    std::wstring ned = needle;
    for (auto& c : ned) if (c >= L'A' && c <= L'Z') c += 32;
    return low.find(ned) != std::wstring::npos;
}
std::wstring resolve_lnk(const std::wstring& lnk_path) {
    std::wstring out;
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&sl))))
        return out;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        if (SUCCEEDED(pf->Load(lnk_path.c_str(), STGM_READ))) {
            wchar_t tgt[MAX_PATH] = {0};
            if (SUCCEEDED(sl->GetPath(tgt, MAX_PATH, nullptr, SLGP_RAWPATH))) out = tgt;
        }
        pf->Release();
    }
    sl->Release();
    return out;
}
std::vector<std::wstring> naraka_lnk_files() {
    std::vector<std::wstring> out;
    wchar_t user_menu[MAX_PATH] = {0}, desktop[MAX_PATH] = {0}, common_menu[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_STARTMENU, nullptr, 0, user_menu);
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop);
    SHGetFolderPathW(nullptr, CSIDL_COMMON_STARTMENU, nullptr, 0, common_menu);
    std::vector<std::wstring> dirs = {user_menu, desktop, common_menu};
    for (auto& d : dirs) {
        if (d.empty() || !dir_exists(d)) continue;
        for (int level = 0; level < 2; level++) {
            std::wstring pat = (fs::path(d) / (level == 0 ? L"*.lnk" : L"*\\*.lnk")).wstring();
            WIN32_FIND_DATAW fd;
            HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
            if (hf == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring name = fd.cFileName;
                if (name.find(L"永劫") != std::wstring::npos || wcsistr_nocase(name, L"naraka"))
                    out.push_back((fs::path(d) / name).wstring());
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    }
    return out;
}
std::wstring exe_from_windows_dir(const std::wstring& wdir) {
    fs::path p = wdir;
    for (int i = 0; i < 6 && p.has_parent_path(); i++) {
        p = p.parent_path();
        std::wstring exe = p.wstring() + L"\\" + GAME_EXE_NAME;
        if (file_exists(exe)) return exe;
    }
    return L"";
}
std::wstring auto_detect_game_exe() {
    // 1) 开始菜单/桌面快捷方式 (官服与 Steam 通吃; 目标可能是包装器 exe, 由目录寻路兜底)
    for (auto& lnk : naraka_lnk_files()) {
        std::wstring tgt = resolve_lnk(lnk);
        if (tgt.empty()) continue;
        std::wstring dir = fs::path(tgt).parent_path().wstring();
        std::wstring w = locate_game_windows(dir);
        if (!w.empty()) {
            std::wstring exe = exe_from_windows_dir(w);
            if (!exe.empty()) return exe;
        }
    }
    // 2) Steam 注册表 + 库目录 (HKCU, 免管理员)
    for (auto& lib : steam_libraries()) {
        std::wstring dir = (fs::path(lib) / L"steamapps" / L"common" / L"Naraka Bladepoint").wstring();
        if (!dir_exists(dir)) continue;
        std::wstring w = locate_game_windows(dir);
        if (!w.empty()) {
            std::wstring exe = exe_from_windows_dir(w);
            if (!exe.empty()) return exe;
        }
    }
    return L"";
}


void save_config() {
    FILE* f = _wfopen(config_file().c_str(), L"wb");
    if (!f) return;
    std::fwrite("\xEF\xBB\xBF", 1, 3, f);
    std::string roles;
    for (const auto& r : g_sel_roles) { if (!roles.empty()) roles += ","; roles += r; }
    std::string s = "gameexe=" + wide_to_utf8(g_sel_game) + "\njp=" + wide_to_utf8(g_sel_jp) +
                    "\nlangref=" + wide_to_utf8(g_langref_file) +
                    "\nroles=" + roles + "\nsys=" + (g_sel_sys ? "1" : "0") + "\n";
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}
void load_config() {
    FILE* f = _wfopen(config_file().c_str(), L"rb");
    if (!f) return;
    std::string s(8192, 0);
    size_t n = std::fread(&s[0], 1, s.size(), f);
    std::fclose(f);
    s.resize(n);
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
    auto line_of = [&](size_t at) {
        size_t e = s.find('\n', at);
        return s.substr(at, e == std::string::npos ? std::string::npos : e - at);
    };
    size_t p;
    if ((p = s.find("gameexe=")) != std::string::npos) g_sel_game = utf8_to_wide(line_of(p + 8));
    else if ((p = s.find("game=")) != std::string::npos) g_sel_game = utf8_to_wide(line_of(p + 5));
    if ((p = s.find("jp=")) != std::string::npos) g_sel_jp = utf8_to_wide(line_of(p + 3));
    if ((p = s.find("langref=")) != std::string::npos) g_langref_file = utf8_to_wide(line_of(p + 8));
    if ((p = s.find("roles=")) != std::string::npos) {
        std::string rl = line_of(p + 6);
        size_t st2 = 0;
        while (st2 <= rl.size()) {
            size_t comma = rl.find(',', st2);
            std::string one = rl.substr(st2, comma == std::string::npos ? std::string::npos : comma - st2);
            if (!one.empty()) g_sel_roles.insert(one);
            if (comma == std::string::npos) break;
            st2 = comma + 1;
        }
    }
    if ((p = s.find("sys=")) != std::string::npos && line_of(p + 4) == "1") g_sel_sys = true;
}

void resolve_all() {
    g_game_ok = g_jp_ok = g_bak_ok = false;
    g_game_win.clear(); g_jp_pck.clear(); g_lang_refs.clear();
    if (!g_sel_game.empty()) {
        std::wstring base = g_sel_game;
        std::wstring ext = fs::path(base).extension().wstring();
        if (_wcsicmp(ext.c_str(), L".exe") == 0) base = fs::path(base).parent_path().wstring();
        g_game_win = locate_game_windows(base);
        g_game_ok = !g_game_win.empty();
    }
    g_bak_ok = g_game_ok && file_exists(bak_path());
    if (!g_sel_jp.empty()) {
        g_lang_refs = find_all_lang_pcks(g_sel_jp);
        for (auto& r : g_lang_refs)
            if (!g_langref_file.empty() && _wcsicmp(r.file.c_str(), g_langref_file.c_str()) == 0) {
                g_jp_pck = r.path; break;
            }
        if (g_jp_pck.empty() && !g_lang_refs.empty()) g_jp_pck = g_lang_refs[0].path;
        g_jp_ok = !g_jp_pck.empty();
    }
}

// ---------- 状态判定 ----------
bool tables_match(const std::wstring& a_path, const std::wstring& b_path) {
    AkpkFile a, b;
    if (!a.open(a_path) || !b.open(b_path)) return false;
    if (a.banks().size() != b.banks().size()) return false;
    for (size_t i = 0; i < a.banks().size(); i++) {
        if (a.banks()[i].id != b.banks()[i].id || a.banks()[i].size != b.banks()[i].size) return false;
    }
    return true;
}
int count_jp_roles() {
    if (!g_game_ok || !g_jp_ok) return 0;
    AkpkFile a, b;
    if (!a.open(pck_path()) || !b.open(g_jp_pck)) return 0;
    int cnt = 0;
    for (const auto& r : ROLE_TABLE) {
        const BankEntry* pa = nullptr; const BankEntry* pb = nullptr;
        for (int c = 0; c < 4 && !pa; c++) {
            uint32_t id = fnv1_str(std::string(BANK_PREFIXES[c]) + r.en);
            const BankEntry* x = a.find_bank(id);
            const BankEntry* y = b.find_bank(id);
            if (x && y && x->has_bkhd && y->has_bkhd && x->size == y->size) { pa = x; pb = y; }
        }
        if (!pa) continue;
        std::vector<uint8_t> vx, vy;
        if (!a.read(pa->body_start, pa->size, vx) || !b.read(pb->body_start, pb->size, vy)) continue;
        vx.erase(vx.begin() + 16, vx.begin() + 20);
        vy.erase(vy.begin() + 16, vy.begin() + 20);
        if (vx == vy) cnt++;
    }
    return cnt;
}
void refresh_state() {
    g_state = "unknown";
    if (g_game_ok && file_exists(pck_path())) {
        if (g_jp_ok && tables_match(pck_path(), g_jp_pck)) g_state = "jp";
        else if (g_bak_ok && tables_match(pck_path(), bak_path())) g_state = "cn";
    }
    g_jp_roles = count_jp_roles();
}

bool game_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, GAME_EXE_NAME) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ---------- 操作 ----------
struct OpResult { bool ok; std::wstring msg; std::wstring detail; };

OpResult op_apply(bool full_mode) {
    std::wstring pck = pck_path(), bak = bak_path(), tmp = tmp_out_path();
    if (!g_game_ok || !file_exists(pck)) return {false, L"游戏语音包未定位, 请完成第一步", L""};
    if (!g_jp_ok || !file_exists(g_jp_pck)) return {false, L"语言包未定位, 请完成第一步", L""};

    // 备份安全策略: .bak 只允许是官方中文, 绝不被我们的构建覆盖。
    // 判定"当前文件是工具构建过的": 部署时写入指纹标记 (ChineseCN.pck.nvt = 部署哈希), 与现文件哈希一致即为自产
    std::wstring cur_hash = utf8_to_wide(sha256_file(pck));
    bool cur_is_ours = (!cur_hash.empty() && marker_read() == cur_hash);
    if (!file_exists(bak)) {
        if (cur_is_ours)
            return {false, L"当前是工具生成的语言包且没有中文备份, 已中止。\n请先用 Steam「验证文件完整性」恢复中文文件, 再使用本工具",
                    L"no backup, current=our build"};
        g_phase = PH_BACKUP;
        if (!copy_file_progress(pck, bak, [](uint64_t d, uint64_t t) {
                g_pct = (int)(d * 50 / t); })) {
            return {false, L"备份中文语音包失败 (磁盘空间/权限/被占用?)",
                    std::wstring(L"backup copy failed:\n  src: ") + pck + L"\n  dst: " + bak};
        }
    } else if (!cur_is_ours) {
        // 当前与上次部署不同 -> 视为官方中文 (可能已随游戏更新) -> 刷新备份
        g_phase = PH_BACKUP;
        if (!copy_file_progress(pck, bak, [](uint64_t d, uint64_t t) {
                g_pct = (int)(d * 50 / t); })) {
            return {false, L"备份中文语音包失败 (磁盘空间/权限/被占用?)",
                    std::wstring(L"backup copy failed:\n  src: ") + pck + L"\n  dst: " + bak};
        }
    }
    g_bak_ok = file_exists(bak);


    // 阶段2: 生成目标语言包 (全量=替换所有 bank; 按角色=仅勾选 bank)
    std::vector<uint32_t> only;
    if (!full_mode) {
        std::vector<uint32_t> role_ids;
        for (const auto& r : ROLE_TABLE) {
            if (!g_sel_roles.count(r.en)) continue;
            for (int c = 0; c < 4; c++) role_ids.push_back(fnv1_str(std::string(BANK_PREFIXES[c]) + r.en));
        }
        if (role_ids.empty() && !g_sel_sys)
            return {false, L"请先勾选要替换的角色 (或切到「全量替换」)", L""};
        AkpkFile src;
        if (src.open(bak)) {
            std::set<uint32_t> rs(role_ids.begin(), role_ids.end());
            for (const auto& cb2 : src.banks()) {
                bool is_role_bank = rs.count(cb2.id) > 0;
                if (is_role_bank || g_sel_sys) only.push_back(cb2.id);
            }
        } else only = role_ids;
    }

    g_phase = PH_BUILD;
    if (file_exists(tmp)) DeleteFileW(tmp.c_str());
    RebuildStats st;
    if (!rebuild_pck(bak, g_jp_pck, tmp, st, [](uint64_t d, uint64_t t) {
            g_pct = 50 + (int)(d * 40 / t); }, only)) {
        std::wstring det;
        for (auto& l : st.log) { det += utf8_to_wide(l); det += L"\n"; }
        DeleteFileW(tmp.c_str());
        return {false, L"生成语言包失败", L"rebuild_pck failed\n" + det};
    }

    g_phase = PH_DEPLOY;
    if (!copy_file_progress(tmp, pck, [](uint64_t d, uint64_t t) {
            g_pct = 90 + (int)(d * 10 / t); })) {
        DeleteFileW(tmp.c_str());
        return {false, L"写入游戏目录失败: 文件可能被占用, 请确认游戏已完全退出",
                std::wstring(L"deploy copy failed:\n  src: ") + tmp + L"\n  dst: " + pck};
    }
    marker_write(utf8_to_wide(sha256_file(tmp)));
    DeleteFileW(tmp.c_str());
    wchar_t b[128];
    std::swprintf(b, 128, L"替换完成 (共 %d 个语音包)", st.banks_replaced);
    return {true, b, L""};
}

OpResult op_restore() {
    std::wstring pck = pck_path(), bak = bak_path();
    if (!g_game_ok || !file_exists(pck)) return {false, L"游戏语音包未定位, 请完成第一步", L""};
    if (!file_exists(bak)) return {false, L"找不到备份 ChineseCN.pck.bak, 无法恢复。\n可用 Steam「验证文件完整性」还原", L""};
    bool is_jp = g_jp_ok && tables_match(pck, g_jp_pck);
    bool is_bak_cn = tables_match(pck, bak);
    if (!is_jp && !is_bak_cn && g_jp_roles <= 0)
        return {true,
                L"当前语音包已是中文原版，且与备份不同 —— 可能游戏刚更新过。\n无需恢复：直接点「开始替换」即可获得目标语言语音（备份也会自动刷新）。",
                L""};
    g_phase = PH_RESTORE;
    if (!copy_file_progress(bak, pck, [](uint64_t d, uint64_t t) {
            g_pct = (int)(d * 100 / t); })) {
        return {false, L"恢复失败: 文件可能被占用, 请确认游戏已完全退出",
                std::wstring(L"restore copy failed:\n  src: ") + bak + L"\n  dst: " + pck};
    }
    DeleteFileW(marker_path().c_str());
    return {true, L"已恢复原版中文语音", L""};
}

void save_error_log(const std::wstring& msg, const std::wstring& detail) {
    FILE* f = _wfopen(error_log_path().c_str(), L"wb");
    if (!f) return;
    std::time_t t = std::time(nullptr);
    char tb[64];
    std::strftime(tb, 64, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::string s = "\xEF\xBB\xBF";
    s += "time: " + std::string(tb) + "\n";
    s += "msg: " + wide_to_utf8(msg) + "\n";
    s += "game: " + wide_to_utf8(g_sel_game) + "\n";
    s += "lang_dir: " + wide_to_utf8(g_sel_jp) + "\n";
    s += "ref: " + wide_to_utf8(g_jp_pck) + "\n";
    s += "detail:\n" + wide_to_utf8(detail) + "\n";
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

// ---------- UI ----------
const COLORREF C_BG    = RGB(16, 20, 28);
const COLORREF C_GRAY  = RGB(186, 193, 206);
const COLORREF C_DISC  = RGB(148, 156, 172);
const COLORREF C_WHITE = RGB(240, 242, 246);
const COLORREF C_FACE  = RGB(46, 54, 70);
const COLORREF C_GREEN = RGB(61, 220, 132);
const COLORREF C_RED   = RGB(255, 99, 99);
const COLORREF C_YEL   = RGB(255, 194, 75);
const COLORREF C_BLUE  = RGB(110, 168, 255);

enum {
    IDC_BTN_GAME = 1101, IDC_BTN_JP = 1102,
    IDC_V_GAME = 1111, IDC_V_JP = 1112,
    IDC_ST_GAME = 1121, IDC_ST_JP = 1122,
    IDC_ST_RUN = 1130,
    IDC_H1 = 1141, IDC_H2 = 1142, IDC_H3 = 1143,
    IDC_ST_CUR = 1144,
    IDC_TAB_FULL = 1151, IDC_TAB_ROLES = 1152,
    IDC_ST_FULLHINT = 1153,
    IDC_PROGRESS = 1160, IDC_ST_PHASE = 1161, IDC_ST_PCT = 1165,
    IDC_BTN_APPLY_FULL = 1162, IDC_BTN_APPLY_ROLES = 1163,
    IDC_BTN_RESTORE = 1164,
    IDC_BTN_ALL = 1340, IDC_BTN_NONE = 1341, IDC_BTN_SYS = 1342,   // 芯片区 1301..1301+N, 功能键留在区间外
    IDC_ST_MSG = 1171, IDC_ST_DISC = 1172,
    IDC_LINE3 = 1190,
    IDC_CHIP_BASE = 1301,   // 1301..1326 = 角色芯片 (26 个), 与上方按钮 ID 不重叠
};
const int WM_OP_DONE = WM_APP + 1;
const int IDT_UI = 1, IDT_POLL = 2;

HWND g_hwnd = nullptr;
// 动态布局 (随 ROLE_TABLE 数量伸缩)
int g_sys_y, g_btn_y, g_applyr_y, g_prog_y, g_phase_y, g_h3_y, g_line_y, g_restore_y, g_msg_y, g_disc_y, g_client_h, g_dy_full;
static void compute_layout() {
    int rows = ((int)ROLE_TABLE.size() + 3) / 4;
    int grid_bottom = 232 + rows * 28;
    g_sys_y = grid_bottom + 8;
    g_btn_y = g_sys_y + 32;
    g_applyr_y = g_btn_y + 36;
    g_prog_y = g_applyr_y + 68;
    g_phase_y = g_prog_y + 22;
    g_h3_y = g_phase_y + 26;
    g_line_y = g_h3_y + 22;
    g_restore_y = g_line_y + 10;
    g_msg_y = g_restore_y + 54;
    g_disc_y = g_msg_y + 26;
    g_client_h = g_disc_y + 64;
    g_dy_full = (306 + 16) - g_prog_y;   // 全量面板底 306, 进度条紧跟其后
}
HBRUSH g_bg = nullptr;
HFONT g_f_head, g_f_lbl, g_f_val, g_f_btn, g_f_small, g_f_msg, g_f_tab;

std::wstring ref_lang_name() {
    std::wstring f = fs::path(g_jp_pck).filename().wstring();
    std::wstring lb = label_for_pck(f);
    return lb.empty() ? L"目标语言" : lb;
}

std::wstring cur_state_short() {
    std::wstring lang = g_jp_ok ? ref_lang_name() : L"目标语言";
    if (g_state == "jp") return lang + L"配音(全部)";
    if (g_jp_roles > 0) return L"部分替换 " + std::to_wstring(g_jp_roles) + L"/" +
                                 std::to_wstring((int)ROLE_TABLE.size()) + L"(" + lang + L")";
        return L"中文原版";
}

std::wstring phase_text() {
    switch (g_phase.load()) {
    case PH_BACKUP:  return L"正在备份中文语音包";
    case PH_BUILD: {
        static const wchar_t* dots[] = {L"", L".", L"..", L"..."};
        return std::wstring(L"正在生成") + ref_lang_name() + L"包" + dots[g_anim % 4];
    }
    case PH_DEPLOY:  return L"正在写入游戏目录";
    case PH_RESTORE: return L"正在恢复原版语音";
    }
    return L"";
}

void refresh_texts() {
    auto setv = [](int id, const std::wstring& t) { SetWindowTextW(GetDlgItem(g_hwnd, id), t.c_str()); };
    setv(IDC_V_GAME, g_sel_game.empty() ? L"(尚未选择, 点右侧选游戏 exe)" : g_sel_game);
    setv(IDC_V_JP,   g_sel_jp.empty()   ? L"(尚未选择)" : g_sel_jp);
    if (g_sel_game.empty()) setv(IDC_ST_GAME, L"");
    else if (g_game_ok) {
        wchar_t b[96];
        std::swprintf(b, 96, L"✔ 已定位语音包 %s (%.0f MB)", PCK_NAME, file_size_of(pck_path()) / 1048576.0);
        setv(IDC_ST_GAME, b);
    } else setv(IDC_ST_GAME, L"✘ 未找到语音包 —— 请确认选的是游戏 exe");
    if (g_sel_jp.empty()) setv(IDC_ST_JP, L"");
    else if (g_jp_ok) {
        std::wstring file = fs::path(g_jp_pck).filename().wstring();
        std::wstring lb = label_for_pck(file);
        wchar_t b[160];
        if (!lb.empty()) std::swprintf(b, 160, L"✔ %s (%s · %.0f MB)", lb.c_str(), file.c_str(),
                                       file_size_of(g_jp_pck) / 1048576.0);
        else std::swprintf(b, 160, L"✔ 已定位 %s (%.0f MB)", file.c_str(),
                           file_size_of(g_jp_pck) / 1048576.0);
        setv(IDC_ST_JP, b);
    } else setv(IDC_ST_JP, L"✘ 没找到语言包 —— 请先解压官方语音包再选目录");

    setv(IDC_ST_RUN, g_running ? L"游戏：● 已启动" : L"游戏：● 未运行");
    setv(IDC_ST_CUR, L"当前: " + cur_state_short());

    bool can_base = !g_busy;
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_APPLY_FULL),
                 can_base && g_game_ok && g_jp_ok && file_exists(pck_path()));
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_APPLY_ROLES),
                 can_base && g_game_ok && g_jp_ok && file_exists(pck_path()) &&
                 (!g_sel_roles.empty() || g_sel_sys));
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_RESTORE), can_base && g_bak_ok);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_GAME), !g_busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_JP), !g_busy);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_APPLY_FULL), nullptr, TRUE);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_APPLY_ROLES), nullptr, TRUE);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_RESTORE), nullptr, TRUE);
}

void apply_tab_layout() {
    bool full = g_tab == 0;
    ShowWindow(GetDlgItem(g_hwnd, IDC_ST_FULLHINT), full ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hwnd, IDC_BTN_APPLY_FULL), full ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < (int)ROLE_TABLE.size(); i++)
        ShowWindow(GetDlgItem(g_hwnd, IDC_CHIP_BASE + i), full ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(g_hwnd, IDC_BTN_SYS), full ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(g_hwnd, IDC_BTN_ALL), full ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(g_hwnd, IDC_BTN_NONE), full ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(g_hwnd, IDC_BTN_APPLY_ROLES), full ? SW_HIDE : SW_SHOW);
    // 动态高度: 全量面板矮 312px, 第三步及以下整体上移
    const int dy = full ? g_dy_full : 0;
    struct Mv { int id; int y; };
    const Mv mv[] = {{IDC_PROGRESS, g_prog_y}, {IDC_ST_PCT, g_prog_y}, {IDC_ST_PHASE, g_phase_y},
                     {IDC_H3, g_h3_y}, {IDC_LINE3, g_line_y}, {IDC_BTN_RESTORE, g_restore_y},
                     {IDC_ST_MSG, g_msg_y}, {IDC_ST_DISC, g_disc_y}};
    for (const auto& e : mv) {
        HWND c = GetDlgItem(g_hwnd, e.id);
        if (!c) continue;
        RECT rc; GetWindowRect(c, &rc);
        MapWindowPoints(nullptr, g_hwnd, (POINT*)&rc, 2);
        SetWindowPos(c, nullptr, rc.left, e.y + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // 窗口高度
    static int h_roles = 0, h_full = 0;
    if (!h_roles) {
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT r1{0, 0, 560, g_client_h}, r2{0, 0, 560, g_client_h + g_dy_full};
        AdjustWindowRect(&r1, style, FALSE);
        AdjustWindowRect(&r2, style, FALSE);
        h_roles = r1.bottom - r1.top;
        h_full = r2.bottom - r2.top;
    }
    RECT wr; GetWindowRect(g_hwnd, &wr);
    int want = full ? h_full : h_roles;
    SetWindowPos(g_hwnd, nullptr, 0, 0, wr.right - wr.left, want, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

// 前置声明
std::wstring pick_folder(HWND owner, const wchar_t* title, const std::wstring& init);
std::wstring pick_lang_popup(HWND owner, const std::vector<LangRef>& cands);

void paint_big_btn(HDC dc, const RECT& rc, bool enabled, COLORREF face) {
    FillRect(dc, &rc, g_bg);
    COLORREF txt = enabled ? RGB(14, 18, 26) : RGB(120, 128, 146);
    if (!enabled) face = RGB(52, 58, 72);
    HBRUSH br = CreateSolidBrush(face);
    HPEN pen = CreatePen(PS_SOLID, 1, face);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 18, 18);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}
void paint_btn_text(HDC dc, const RECT& rc, HWND item, COLORREF txt, HFONT f) {
    wchar_t t[160] = {0};
    GetWindowTextW(item, t, 160);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, txt);
    HGDIOBJ of = SelectObject(dc, f);
    DrawTextW(dc, t, -1, (RECT*)&rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SelectObject(dc, of);
}

// 多语言包选择弹窗 (模态)
struct PickCtx { std::vector<LangRef> cands; std::wstring result; };
PickCtx* g_pick = nullptr;

LRESULT CALLBACK pickproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CREATE) {
        int n = (int)g_pick->cands.size();
        auto st = CreateWindowW(L"STATIC", L"检测到多个语言包, 请选择要用哪个:",
                                WS_CHILD | WS_VISIBLE, 20, 14, 460, 20, h, nullptr, nullptr, nullptr);
        SendMessageW(st, WM_SETFONT, (WPARAM)g_f_lbl, TRUE);
        for (int i = 0; i < n; i++) {
            std::wstring label = g_pick->cands[i].label.empty()
                ? g_pick->cands[i].file
                : g_pick->cands[i].label + L" — " + g_pick->cands[i].file;
            HWND b = CreateWindowW(L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   20, 44 + i * 44, 460, 36, h, (HMENU)(INT_PTR)(2000 + i), nullptr, nullptr);
            SendMessageW(b, WM_SETFONT, (WPARAM)g_f_val, TRUE);
        }
        return 0;
    }
    if (m == WM_DRAWITEM) {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)l;
        bool hover_ish = !(d->itemState & ODS_DISABLED);
        FillRect(d->hDC, &d->rcItem, g_bg);
        COLORREF face = (d->itemState & ODS_SELECTED) ? RGB(34, 120, 80) : RGB(36, 42, 56);
        HBRUSH br = CreateSolidBrush(face);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(52, 60, 76));
        HGDIOBJ ob = SelectObject(d->hDC, br), op = SelectObject(d->hDC, pen);
        RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right - 1, d->rcItem.bottom - 1, 12, 12);
        SelectObject(d->hDC, ob); SelectObject(d->hDC, op);
        DeleteObject(br); DeleteObject(pen);
        wchar_t t[160] = {0};
        GetWindowTextW(d->hwndItem, t, 160);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, C_WHITE);
        HGDIOBJ of = SelectObject(d->hDC, g_f_val);
        DrawTextW(d->hDC, t, -1, &d->rcItem, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(d->hDC, of);
        return TRUE;
    }
    if (m == WM_COMMAND && HIWORD(w) == BN_CLICKED) {
        int id = LOWORD(w);
        if (g_pick && id >= 2000 && id < 2000 + (int)g_pick->cands.size()) {
            g_pick->result = g_pick->cands[id - 2000].path;
            DestroyWindow(h);
        }
        return 0;
    }
    if (m == WM_CTLCOLORSTATIC) {
        SetBkMode((HDC)w, TRANSPARENT);
        SetTextColor((HDC)w, C_GRAY);
        return (LRESULT)g_bg;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring pick_lang_popup(HWND owner, const std::vector<LangRef>& cands) {
    static bool cls_ok = false;
    if (!cls_ok) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = pickproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = g_bg;
        wc.lpszClassName = L"NarakaPickLang";
        RegisterClassW(&wc);
        cls_ok = true;
    }
    PickCtx ctx{cands, L""};
    g_pick = &ctx;
    int n = (int)cands.size();
    RECT r{0, 0, 500, 60 + n * 44 + 14};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND p = CreateWindowW(L"NarakaPickLang", L"选择语言包",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!p) { g_pick = nullptr; return L""; }
    EnableWindow(owner, FALSE);
    ShowWindow(p, SW_SHOW);
    UpdateWindow(p);
    MSG msg;
    while (IsWindow(p) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_pick = nullptr;
    return ctx.result;
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        compute_layout();
        auto st = [&](int id, const wchar_t* txt, int x, int y, int wd, int ht,
                      HFONT f, DWORD style = SS_LEFT) {
            HWND c = CreateWindowW(L"STATIC", txt, WS_CHILD | WS_VISIBLE | style,
                                   x, y, wd, ht, h, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            return c;
        };
        auto bt = [&](int id, const wchar_t* txt, int x, int y, int wd, int ht, DWORD style) {
            HWND c = CreateWindowW(L"BUTTON", txt, WS_CHILD | WS_VISIBLE | style,
                                   x, y, wd, ht, h, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_f_lbl, TRUE);
            return c;
        };
        st(IDC_H1, L"第一步 · 设置路径", 24, 14, 240, 20, g_f_head);
        st(IDC_ST_RUN, L"● 未运行", 300, 14, 236, 20, g_f_val, SS_RIGHT);
        st(0, L"", 24, 38, 512, 2, g_f_lbl, SS_ETCHEDHORZ);
        st(0, L"游戏 exe", 24, 48, 80, 18, g_f_lbl);
        st(IDC_V_GAME, L"(尚未选择)", 110, 44, 426, 22, g_f_val, SS_PATHELLIPSIS);
        bt(IDC_BTN_GAME, L"选 exe", 444, 68, 92, 24, BS_OWNERDRAW);
        st(IDC_ST_GAME, L"", 110, 74, 326, 16, g_f_small);
        st(0, L"语言包目录", 24, 96, 80, 18, g_f_lbl);
        st(IDC_V_JP, L"(尚未选择)", 110, 92, 426, 22, g_f_val, SS_PATHELLIPSIS);
        bt(IDC_BTN_JP, L"选目录", 444, 116, 92, 24, BS_OWNERDRAW);
        st(IDC_ST_JP, L"", 110, 122, 326, 16, g_f_small);

        st(IDC_H2, L"第二步 · 替换语音包", 24, 146, 240, 20, g_f_head);
        st(IDC_ST_CUR, L"当前: 未知", 300, 146, 236, 20, g_f_small, SS_RIGHT);
        st(0, L"", 24, 170, 512, 2, g_f_lbl, SS_ETCHEDHORZ);
        CreateWindowW(L"BUTTON", L"全量替换", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, 180, 140, 28, h, (HMENU)(INT_PTR)IDC_TAB_FULL, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"按角色替换", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      168, 180, 140, 28, h, (HMENU)(INT_PTR)IDC_TAB_ROLES, nullptr, nullptr);

        // 全量面板
        st(IDC_ST_FULLHINT, L"把全部角色与系统语音替换为所选语言（等价于官方完整语言切换）",
           24, 224, 512, 18, g_f_lbl);
        CreateWindowW(L"BUTTON", L"开始替换 (自动备份中文语音)", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, 252, 512, 54, h, (HMENU)(INT_PTR)IDC_BTN_APPLY_FULL, nullptr, nullptr);

        // 按角色面板
        for (int i = 0; i < (int)ROLE_TABLE.size(); i++) {
            int col = i % 4, row = i / 4;
            CreateWindowW(L"BUTTON", utf8_to_wide(ROLE_TABLE[i].cn).c_str(),
                          WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                          24 + col * 126, 232 + row * 28, 122, 24,
                          h, (HMENU)(INT_PTR)(IDC_CHIP_BASE + i), nullptr, nullptr);
        }
        CreateWindowW(L"BUTTON", L"其他/系统语音 (击杀播报等非角色内容)", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, g_sys_y, 470, 24, h, (HMENU)(INT_PTR)IDC_BTN_SYS, nullptr, nullptr);
        bt(IDC_BTN_ALL, L"全选", 24, g_btn_y, 110, 26, BS_OWNERDRAW);
        bt(IDC_BTN_NONE, L"清空", 142, g_btn_y, 110, 26, BS_OWNERDRAW);
        CreateWindowW(L"BUTTON", L"开始替换 (自动备份中文语音)", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, g_applyr_y, 512, 54, h, (HMENU)(INT_PTR)IDC_BTN_APPLY_ROLES, nullptr, nullptr);

        CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                      24, g_prog_y, 430, 18, h, (HMENU)(INT_PTR)IDC_PROGRESS, nullptr, nullptr);
        st(IDC_ST_PCT, L"0%", 462, g_prog_y, 74, 18, g_f_val);
        SendMessageW(GetDlgItem(h, IDC_PROGRESS), PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(GetDlgItem(h, IDC_PROGRESS), PBM_SETBARCOLOR, 0, (LPARAM)C_GREEN);
        SendMessageW(GetDlgItem(h, IDC_PROGRESS), PBM_SETBKCOLOR, 0, (LPARAM)RGB(40, 46, 60));
        st(IDC_ST_PHASE, L"", 24, g_phase_y, 512, 16, g_f_msg);

        st(IDC_H3, L"第三步 · 恢复原版", 24, g_h3_y, 240, 20, g_f_head);
        st(IDC_LINE3, L"", 24, g_line_y, 512, 2, g_f_lbl, SS_ETCHEDHORZ);
        CreateWindowW(L"BUTTON", L"恢复原版中文语音", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, g_restore_y, 512, 46, h, (HMENU)(INT_PTR)IDC_BTN_RESTORE, nullptr, nullptr);

        st(IDC_ST_MSG, L"", 24, g_msg_y, 512, 20, g_f_msg);
        st(IDC_ST_DISC,
           L"仅供个人学习研究使用; 只替换本地音频文件, 不修改游戏程序; 请勿分发语音包 (含版权音频)。\n"
           L"失败时弹窗报错可 Ctrl+C 复制, 详细日志在工具目录 gui_error.log。游戏更新后重新「开始替换」即可。",
           24, g_disc_y, 512, 56, g_f_small);
        return 0;
    }
    case WM_OP_DONE: {
        g_busy = false;
        g_phase = PH_IDLE;
        if (g_op_ok) {
            wchar_t b[64];
            std::swprintf(b, 64, L"完成 100%%");
            SetWindowTextW(GetDlgItem(h, IDC_ST_PHASE), b);
            SetWindowTextW(GetDlgItem(h, IDC_ST_MSG), g_op_msg.c_str());
            refresh_state();
            refresh_texts();
            MessageBoxW(h, (g_op_msg + L"\n\n(若游戏已开着, 重启游戏后生效)").c_str(),
                        L"操作成功", MB_OK | MB_ICONINFORMATION);
        } else {
            save_error_log(g_op_msg, g_op_detail);
            std::wstring box = g_op_msg + L"\n\n──────────\n" + g_op_detail +
                L"\n详细日志已保存: " + error_log_path() +
                L"\n(在弹窗上按 Ctrl+C 可复制全部内容, 连同日志文件一起发给排查)";
            SetWindowTextW(GetDlgItem(h, IDC_ST_MSG), (L"[失败] " + g_op_msg).c_str());
            refresh_texts();
            MessageBoxW(h, box.c_str(), L"操作失败", MB_OK | MB_ICONERROR);
            SendMessageW(GetDlgItem(h, IDC_PROGRESS), PBM_SETPOS, 0, 0);
            SetWindowTextW(GetDlgItem(h, IDC_ST_PHASE), L"");
        }
        return 0;
    }
    case WM_TIMER:
        if (w == IDT_UI) {
            wchar_t b[32];
            std::swprintf(b, 32, L"%d%%", g_pct.load());
            SetWindowTextW(GetDlgItem(h, IDC_ST_PCT), b);
            if (g_busy) {
                SendMessageW(GetDlgItem(h, IDC_PROGRESS), PBM_SETPOS, g_pct.load(), 0);
                wchar_t b2[128];
                std::swprintf(b2, 128, L"%s  %d%%", phase_text().c_str(), g_pct.load());
                SetWindowTextW(GetDlgItem(h, IDC_ST_PHASE), b2);
                g_anim++;
            }
        } else if (w == IDT_POLL) {
            g_running = game_running();
            if (!g_busy) refresh_texts();
        }
        return 0;
    case WM_CTLCOLORBTN:
        return (LRESULT)g_bg;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_GRAY);
        switch (GetDlgCtrlID((HWND)l)) {
        case IDC_H1: case IDC_H2: case IDC_H3: SetTextColor(dc, C_WHITE); break;
        case IDC_V_GAME: case IDC_V_JP: SetTextColor(dc, C_WHITE); break;
        case IDC_ST_RUN: SetTextColor(dc, g_running ? C_RED : C_GREEN); break;
        case IDC_ST_GAME: SetTextColor(dc, g_game_ok ? C_GREEN : C_RED); break;
        case IDC_ST_JP:   SetTextColor(dc, g_jp_ok ? C_GREEN : C_RED); break;
        case IDC_ST_CUR:  SetTextColor(dc, C_GRAY); break;
        case IDC_ST_PHASE: SetTextColor(dc, C_GREEN); break;
        case IDC_ST_MSG: SetTextColor(dc, g_op_ok ? C_GREEN : C_RED); break;
        case IDC_ST_DISC: SetTextColor(dc, C_DISC); break;
        case IDC_ST_FULLHINT: SetTextColor(dc, C_GRAY); break;
        }
        return (LRESULT)g_bg;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)l;
        int id = d->CtlID;
        // Tab 页签: 选中 = 白字+绿色底条, 未选 = 灰字
        if (id == IDC_TAB_FULL || id == IDC_TAB_ROLES) {
            FillRect(d->hDC, &d->rcItem, g_bg);
            bool sel = (g_tab == 0) == (id == IDC_TAB_FULL);
            COLORREF face = sel ? RGB(30, 36, 50) : RGB(22, 27, 38);
            HBRUSH br = CreateSolidBrush(face);
            HPEN pen = CreatePen(PS_SOLID, 1, sel ? C_GREEN : RGB(44, 52, 68));
            HGDIOBJ ob = SelectObject(d->hDC, br), op = SelectObject(d->hDC, pen);
            if (sel) {
                RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right - 1, d->rcItem.bottom - 1, 10, 10);
                // 绿色底条
                HBRUSH gb = CreateSolidBrush(C_GREEN);
                RECT bar = {d->rcItem.left + 10, d->rcItem.bottom - 5, d->rcItem.right - 11, d->rcItem.bottom - 2};
                FillRect(d->hDC, &bar, gb);
                DeleteObject(gb);
            } else {
                RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right - 1, d->rcItem.bottom - 1, 10, 10);
            }
            SelectObject(d->hDC, ob); SelectObject(d->hDC, op);
            DeleteObject(br); DeleteObject(pen);
            wchar_t t[64] = {0};
            GetWindowTextW(d->hwndItem, t, 64);
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, sel ? C_WHITE : C_GRAY);
            HGDIOBJ of = SelectObject(d->hDC, g_f_tab);
            DrawTextW(d->hDC, t, -1, &d->rcItem, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            SelectObject(d->hDC, of);
            return TRUE;
        }
        // 角色芯片 / 系统芯片
        if ((id >= IDC_CHIP_BASE && id < IDC_CHIP_BASE + (int)ROLE_TABLE.size()) || id == IDC_BTN_SYS) {
            FillRect(d->hDC, &d->rcItem, g_bg);
            bool sel = id == IDC_BTN_SYS ? g_sel_sys
                                         : g_sel_roles.count(ROLE_TABLE[id - IDC_CHIP_BASE].en) > 0;
            bool enabled = !(d->itemState & ODS_DISABLED);
            COLORREF face = sel ? RGB(34, 120, 80) : RGB(36, 42, 56);
            COLORREF txt  = sel ? RGB(190, 255, 220) : C_GRAY;
            if (!enabled) { face = RGB(30, 34, 44); txt = RGB(80, 88, 104); }
            HBRUSH br = CreateSolidBrush(face);
            HPEN pen = CreatePen(PS_SOLID, 1, sel ? C_GREEN : RGB(52, 60, 76));
            HGDIOBJ ob = SelectObject(d->hDC, br), op = SelectObject(d->hDC, pen);
            RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right - 1, d->rcItem.bottom - 1, 12, 12);
            SelectObject(d->hDC, ob); SelectObject(d->hDC, op);
            DeleteObject(br); DeleteObject(pen);
            wchar_t t[128] = {0};
            GetWindowTextW(d->hwndItem, t, 128);
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, txt);
            HGDIOBJ of = SelectObject(d->hDC, g_f_lbl);
            RECT rc = d->rcItem; rc.left += 6;
            DrawTextW(d->hDC, t, -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            SelectObject(d->hDC, of);
            return TRUE;
        }
        // 小按钮
        if (id == IDC_BTN_GAME || id == IDC_BTN_JP || id == IDC_BTN_ALL || id == IDC_BTN_NONE) {
            bool enabled = !(d->itemState & ODS_DISABLED);
            bool all_sel = true, any_sel = false;
            for (const auto& r : ROLE_TABLE) if (g_sel_roles.count(r.en)) { any_sel = true; break; }
            for (const auto& r : ROLE_TABLE) if (!g_sel_roles.count(r.en)) { all_sel = false; break; }
            COLORREF face = enabled ? RGB(52, 64, 94) : RGB(32, 37, 48);
            COLORREF txt  = enabled ? RGB(182, 208, 255) : RGB(96, 104, 120);
            if (id == IDC_BTN_ALL && enabled) {
                if (all_sel)      { face = RGB(34, 120, 80); txt = RGB(190, 255, 220); }
                else if (any_sel) { face = RGB(96, 84, 40);  txt = RGB(255, 226, 150); }
            }
            paint_big_btn(d->hDC, d->rcItem, enabled, face);
            paint_btn_text(d->hDC, d->rcItem, d->hwndItem, txt, g_f_lbl);
            return TRUE;
        }
        // 大按钮
        if (id == IDC_BTN_APPLY_FULL || id == IDC_BTN_APPLY_ROLES || id == IDC_BTN_RESTORE) {
            bool enabled = !(d->itemState & ODS_DISABLED);
            COLORREF face = id == IDC_BTN_RESTORE ? C_BLUE : C_GREEN;
            paint_big_btn(d->hDC, d->rcItem, enabled, face);
            paint_btn_text(d->hDC, d->rcItem, d->hwndItem,
                           enabled ? RGB(14, 18, 26) : RGB(120, 128, 146), g_f_btn);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED && !g_busy) {
            int id = LOWORD(w);
            if (id == IDC_TAB_FULL || id == IDC_TAB_ROLES) {
                g_tab = id == IDC_TAB_FULL ? 0 : 1;
                apply_tab_layout();
            } else if (id >= IDC_CHIP_BASE && id < IDC_CHIP_BASE + (int)ROLE_TABLE.size()) {
                const std::string& en = ROLE_TABLE[id - IDC_CHIP_BASE].en;
                if (g_sel_roles.count(en)) g_sel_roles.erase(en); else g_sel_roles.insert(en);
                save_config(); refresh_texts();
                InvalidateRect((HWND)l, nullptr, TRUE);
                InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_ALL), nullptr, TRUE);
            } else if (id == IDC_BTN_ALL) {
                bool all_sel = true;
                for (const auto& r : ROLE_TABLE) if (!g_sel_roles.count(r.en)) { all_sel = false; break; }
                if (all_sel) g_sel_roles.clear();
                else for (const auto& r : ROLE_TABLE) g_sel_roles.insert(r.en);
                save_config(); refresh_texts();
                InvalidateRect(h, nullptr, TRUE);
            } else if (id == IDC_BTN_NONE) {
                g_sel_roles.clear(); g_sel_sys = false;
                save_config(); refresh_texts();
                InvalidateRect(h, nullptr, TRUE);
            } else if (id == IDC_BTN_SYS) {
                g_sel_sys = !g_sel_sys;
                save_config(); refresh_texts();
                InvalidateRect((HWND)l, nullptr, TRUE);
                InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_ALL), nullptr, TRUE);
            } else if (id == IDC_BTN_GAME) {
                wchar_t file[MAX_PATH] = L"";
                OPENFILENAMEW of{};
                of.lStructSize = sizeof(of);
                of.hwndOwner = h;
                of.lpstrFilter = L"游戏主程序 (NarakaBladepoint.exe)\0NarakaBladepoint.exe\0所有程序\0*.exe\0";
                of.lpstrFile = file;
                of.nMaxFile = MAX_PATH;
                of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (!g_sel_game.empty()) {
                    fs::path initp(g_sel_game);
                    std::wstring initd = fs::path(g_sel_game).extension().wstring() == L".exe"
                        ? initp.parent_path().wstring() : g_sel_game;
                    of.lpstrInitialDir = initd.c_str();
                }
                if (GetOpenFileNameW(&of)) {
                    g_sel_game = file;
                    save_config(); resolve_all(); refresh_state();
                    g_op_ok = true;
                    SetWindowTextW(GetDlgItem(h, IDC_ST_MSG), L"");
                    refresh_texts();
                }
            } else if (id == IDC_BTN_JP) {
                std::wstring sel = pick_folder(h, L"选择语言包目录 (内含 Japanese/English/Thai.pck 等)", g_sel_jp);
                if (!sel.empty()) {
                    g_sel_jp = sel;
                    auto refs = find_all_lang_pcks(sel);
                    std::wstring chosen;
                    if (refs.size() == 1) chosen = refs[0].path;
                    else if (refs.size() >= 2) chosen = pick_lang_popup(h, refs);
                    if (!chosen.empty()) {
                        g_jp_pck = chosen;
                        g_langref_file = fs::path(chosen).filename().wstring();
                        g_jp_ok = true;
                    }
                    save_config(); resolve_all(); refresh_state();
                    g_op_ok = true;
                    SetWindowTextW(GetDlgItem(h, IDC_ST_MSG), L"");
                    refresh_texts();
                }
            } else if (id == IDC_BTN_APPLY_FULL || id == IDC_BTN_APPLY_ROLES) {
                if (game_running()) {
                    MessageBoxW(h, L"游戏正在运行，请先完全退出游戏再替换。\n(替换需要写入游戏语音包文件)", L"无法替换", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                SetWindowTextW(GetDlgItem(h, IDC_ST_MSG), L"");
                g_busy = true; g_pct = 0; g_phase = PH_BACKUP;
                refresh_texts();
                bool full = id == IDC_BTN_APPLY_FULL;
                std::thread([full] {
                    OpResult r = op_apply(full);
                    g_op_ok = r.ok; g_op_msg = r.msg; g_op_detail = r.detail;
                    PostMessageW(g_hwnd, WM_OP_DONE, 0, 0);
                }).detach();
            } else if (id == IDC_BTN_RESTORE) {
                if (game_running()) {
                    MessageBoxW(h, L"游戏正在运行，请先完全退出游戏再恢复。", L"无法恢复", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                SetWindowTextW(GetDlgItem(h, IDC_ST_MSG), L"");
                g_busy = true; g_pct = 0; g_phase = PH_RESTORE;
                refresh_texts();
                std::thread([] {
                    OpResult r = op_restore();
                    g_op_ok = r.ok; g_op_msg = r.msg; g_op_detail = r.detail;
                    PostMessageW(g_hwnd, WM_OP_DONE, 0, 0);
                }).detach();
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring pick_folder(HWND owner, const wchar_t* title, const std::wstring& init) {
    std::wstring result;
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileDialog, (void**)&fd)))
        return result;
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    fd->SetTitle(title);
    if (!init.empty() && dir_exists(init)) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(init.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            fd->SetFolder(si);
            si->Release();
        }
    }
    if (SUCCEEDED(fd->Show(owner))) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(fd->GetResult(&psi))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            psi->Release();
        }
    }
    fd->Release();
    return result;
}

} // namespace

int run_gui() {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    g_bg = CreateSolidBrush(C_BG);
    g_f_head = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_f_lbl  = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_f_val  = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_f_btn  = CreateFontW(-18, 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_f_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_f_msg  = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_f_tab  = CreateFontW(-14, 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg;
    wc.lpszClassName = L"NarakaVoiceGui";
    RegisterClassW(&wc);

    compute_layout();
    RECT r{0, 0, 560, g_client_h};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND h = CreateWindowW(wc.lpszClassName, L"永劫无间 · 语音切换助手",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) return 1;

    load_config();
    if (g_sel_game.empty()) g_sel_game = auto_detect_game_exe();
    if (g_sel_game.empty()) {
        static const char* cands[] = {
            "E:/Naraka/program/NarakaBladepoint.exe",
            "C:/Program Files (x86)/Steam/steamapps/common/Naraka Bladepoint/program/NarakaBladepoint.exe",
            "D:/Steam/steamapps/common/Naraka Bladepoint/program/NarakaBladepoint.exe",
            "E:/Steam/steamapps/common/Naraka Bladepoint/program/NarakaBladepoint.exe",
        };
        for (const char* c : cands)
            if (file_exists(utf8_to_wide(c))) { g_sel_game = utf8_to_wide(c); break; }
    }
    resolve_all();
    refresh_state();
    g_running = game_running();
    refresh_texts();
    apply_tab_layout();

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    SetTimer(h, IDT_UI, 120, nullptr);
    SetTimer(h, IDT_POLL, 2000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}

} // namespace nvtool
