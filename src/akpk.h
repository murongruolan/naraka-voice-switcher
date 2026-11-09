// akpk.h — Naraka Wwise pck (AKPK) 解析核心
// 格式知识来自实测逆向 (2026-11-09):
//   data_start = 0x54 + bank_cnt*20 (头区即 bank 表)
//   body 按序排列, 起点 align_up(累加, 4096); 272/272 BKHD 命中验证
//   bank entry 20B: id / off(恒0x1000) / size / A(wem起始索引) / rsv
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>

namespace nvtool {

struct BankEntry {
    uint32_t id = 0;        // FNV-1(bank名)
    uint32_t off = 0;       // 恒 0x1000, 占位
    uint32_t size = 0;      // body 字节数
    uint32_t a = 0;         // 全局 wem 起始索引
    uint32_t rsv = 0;
    uint64_t body_start = 0;// 对齐后绝对位置
    bool     has_bkhd = false;
};

struct MediaEntry {
    uint32_t id = 0;        // media id (bank 内唯一)
    uint64_t abs_pos = 0;   // 文件内绝对位置
    uint32_t size = 0;
};

struct DidxChunk {
    uint64_t didx_pos = 0;  // DIDX chunk 头位置
    uint32_t didx_size = 0;
    uint64_t data_start = 0;// DATA 数据起点 (DIDX 结束 + 8)
    std::vector<MediaEntry> media;
};

class AkpkFile {
public:
    ~AkpkFile() { close(); }
    bool open(const std::wstring& path);
    void close();

    const std::vector<BankEntry>& banks() const { return banks_; }
    uint32_t data_start() const { return data_start_; }
    uint64_t file_size() const { return file_size_; }
    const std::wstring& path() const { return path_; }

    const BankEntry* find_bank(uint32_t id) const;
    // 解析某 bank 的 chunk 链 -> DIDX/DATA (读文件, 每次调用独立)
    bool parse_didx(const BankEntry& b, DidxChunk& out) const;
    // 读取任意区段
    bool read(uint64_t pos, uint32_t size, std::vector<uint8_t>& out) const;

    static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }
    static uint32_t fnv1(const std::string& s);   // bank 名 -> id

private:
    std::wstring path_;
    FILE* f_ = nullptr;
    uint64_t file_size_ = 0;
    uint32_t data_start_ = 0;
    std::vector<BankEntry> banks_;
    std::unordered_map<uint32_t, size_t> id2idx_;
};

} // namespace nvtool
