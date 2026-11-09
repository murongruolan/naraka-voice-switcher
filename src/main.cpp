// main.cpp — Naraka Voice Tool 入口
// M1: CLI 验证模式 (dump / roles / verify / replace)
// M3: 无参数(双击) -> 原生 GUI
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup")
#include "akpk.h"
#include "roles.h"
#include "rebuild.h"
#include "gui.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

using namespace nvtool;

static void print_usage() {
    std::printf(
        "Naraka Voice Tool (永劫语音切换器) M1\n"
        "用法:\n"
        "  NarakaVoiceTool dump  <pck>   解析结构摘要\n"
        "  NarakaVoiceTool roles <pck>   列出已识别角色 bank\n"
        "  NarakaVoiceTool verify <pck>  校验对齐公式\n"
        "  NarakaVoiceTool gui           启动图形界面 (一键切换/恢复语音)\n");
}

static int cmd_dump(const std::wstring& path) {
    AkpkFile f;
    if (!f.open(path)) { std::printf("[错误] 无法打开或不是 AKPK\n"); return 1; }
    const auto& banks = f.banks();
    int n_bkhd = 0;
    for (auto& b : banks) if (b.has_bkhd) n_bkhd++;
    std::printf("大小: %.1f MB\n", f.file_size() / 1048576.0);
    std::printf("bank 总数: %zu (data_start=0x%X)\n", banks.size(), f.data_start());
    std::printf("有 BKHD 的 bank: %d\n", n_bkhd);
    size_t n_didx = 0, n_media = 0;
    for (auto& b : banks) {
        DidxChunk d;
        if (f.parse_didx(b, d)) { n_didx++; n_media += d.media.size(); }
    }
    std::printf("可解析 DIDX 的 bank: %zu (媒体总数 %zu)\n", n_didx, n_media);
    return 0;
}

static int cmd_roles(const std::wstring& path) {
    AkpkFile f;
    if (!f.open(path)) { std::printf("[错误] 无法打开\n"); return 1; }
    std::printf("%-10s %-11s %-8s %-8s %-8s %-8s %s\n",
                "角色", "id", "ingame", "outgame", "marker", "voice", "总计");
    for (auto& role : ROLE_TABLE) {
        int cnt[4] = {0, 0, 0, 0};
        bool any = false;
        int total = 0;
        for (int c = 0; c < 4; c++) {
            uint32_t id = fnv1_str(std::string(BANK_PREFIXES[c]) + role.en);
            const BankEntry* b = f.find_bank(id);
            if (!b) { cnt[c] = 0; continue; }
            DidxChunk d;
            if (f.parse_didx(*b, d)) { cnt[c] = (int)d.media.size(); }
            else { cnt[c] = -1; }   // 在表但 body 不可解析
        }
        for (int c = 0; c < 4; c++) {
            if (cnt[c] > 0) { any = true; total += cnt[c]; }
        }
        if (!any) continue;   // 只打印命中的角色
        std::printf("%-10s %-11s", role.cn, role.en);
        for (int c = 0; c < 4; c++) {
            if (cnt[c] < 0) std::printf("%-8s", "不可解析");
            else std::printf("%-8s", cnt[c] ? std::to_string(cnt[c]).c_str() : "-");
        }
        std::printf("%d\n", total);
    }
    return 0;
}

static int cmd_verify(const std::wstring& path) {
    AkpkFile f;
    if (!f.open(path)) { std::printf("[错误] 无法打开\n"); return 1; }
    int n_bkhd = 0, n_ok_didx = 0, n_misalign = 0;
    uint64_t acc = f.data_start();
    for (size_t i = 0; i < f.banks().size(); i++) {
        const BankEntry& b = f.banks()[i];
        uint64_t expect = AkpkFile::align_up(acc, 4096);
        if (expect != b.body_start) {
            n_misalign++;
            std::printf("[不一致] bank[%zu] 0x%llX != 0x%llX\n",
                        i, (unsigned long long)b.body_start, (unsigned long long)expect);
        }
        acc = b.body_start + b.size;
        if (b.has_bkhd) n_bkhd++;
        DidxChunk d;
        if (f.parse_didx(b, d)) n_ok_didx++;
    }
    std::printf("对齐校验: 错位 %d 处 | BKHD %d | DIDX 成功 %d\n", n_misalign, n_bkhd, n_ok_didx);
    return 0;
}

static int cmd_replace(const std::wstring& cn, const std::wstring& jp, const std::wstring& out) {
    RebuildStats st;
    std::printf("替换中...\n");
    if (!rebuild_pck(cn, jp, out, st)) {
        for (auto& l : st.log) std::printf("[错误] %s\n", l.c_str());
        return 1;
    }
    std::printf("bank 总数 %d | 替换 %d | 保留(数量不等) %d | 保留(无body) %d | 异常 %d\n",
                st.banks_total, st.banks_replaced, st.banks_kept_mismatch,
                st.banks_kept_nobody, st.banks_kept_error);
    std::printf("替换媒体: %llu 条 | 输出 %.1f MB\n",
                (unsigned long long)st.media_replaced, st.bytes_written / 1048576.0);
    for (auto& l : st.log) std::printf("  %s\n", l.c_str());
    std::string h = sha256_file(out);
    std::printf("输出 SHA256: %s\n", h.c_str());
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return run_gui();   // 双击/无参数 = 直接进图形界面
    std::wstring cmd = argv[1];
    if (cmd == L"dump" && argc >= 3)   return cmd_dump(argv[2]);
    if (cmd == L"roles" && argc >= 3)  return cmd_roles(argv[2]);
    if (cmd == L"verify" && argc >= 3) return cmd_verify(argv[2]);
    if (cmd == L"replace" && argc >= 5) return cmd_replace(argv[2], argv[3], argv[4]);
    if (cmd == L"gui")    return run_gui();
    print_usage();
    return 0;
}
