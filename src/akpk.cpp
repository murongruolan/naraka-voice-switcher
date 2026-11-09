// akpk.cpp — AKPK 解析实现
#include "akpk.h"
#include <algorithm>
#include <cstring>

namespace nvtool {

uint32_t AkpkFile::fnv1(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {           // 已假设调用方传入小写
        h = h * 16777619u;
        h ^= c;
    }
    return h;
}

bool AkpkFile::open(const std::wstring& p) {
    close();
    path_ = p;
#if defined(_WIN32)
    f_ = _wfopen(p.c_str(), L"rb");
#else
    f_ = std::fopen(std::string(p.begin(), p.end()).c_str(), "rb");
#endif
    if (!f_) return false;
    _fseeki64(f_, 0, SEEK_END);
    file_size_ = (uint64_t)_ftelli64(f_);
    _fseeki64(f_, 0, SEEK_SET);

    uint8_t head[0x54];
    if (std::fread(head, 1, sizeof(head), f_) != sizeof(head)) return false;
    if (std::memcmp(head, "AKPK", 4) != 0) return false;

    data_start_ = *(uint32_t*)(head + 4);
    // version @0x08 = 1 (不校验, 官方可能升版本)

    // 动态定位 bank 表 (语言字符串长度可变, 表位置不固定):
    // 规律: entries + cnt*20 == data_start (CN/JP 实测均成立)
    uint64_t ent_pos = 0; uint32_t bank_cnt = 0;
    for (uint64_t c = 0x0C; c + 4 <= data_start_ && c < 0x400; c += 4) {
        uint64_t rem = data_start_ - c;
        if (rem == 0 || rem % 20) continue;
        uint32_t n = (uint32_t)(rem / 20);
        if (n < 100 || n > 100000) continue;
        uint32_t cnt_field = 0;
        {
            std::vector<uint8_t> tmp;
            if (!read(c - 4, 4, tmp)) continue;
            cnt_field = *(uint32_t*)tmp.data();
        }
        if (cnt_field != n) continue;
        bool nz = true;
        for (int k = 0; k < 3; k++) {
            std::vector<uint8_t> tmp;
            if (!read(c + (uint64_t)k * 20, 4, tmp) || *(uint32_t*)tmp.data() == 0) { nz = false; break; }
        }
        if (nz) { ent_pos = c; bank_cnt = n; break; }
    }
    if (!bank_cnt) return false;

    banks_.resize(bank_cnt);
    std::vector<uint8_t> tbl((size_t)bank_cnt * 20);
    if (!read(ent_pos, (uint32_t)tbl.size(), tbl)) return false;

    uint64_t acc = data_start_;
    for (uint32_t i = 0; i < bank_cnt; i++) {
        const uint8_t* e = tbl.data() + (size_t)i * 20;
        BankEntry& b = banks_[i];
        b.id  = *(uint32_t*)(e + 0);
        b.off = *(uint32_t*)(e + 4);
        b.size = *(uint32_t*)(e + 8);
        b.a   = *(uint32_t*)(e + 12);
        b.rsv = *(uint32_t*)(e + 16);
        b.body_start = align_up(acc, 4096);
        acc = b.body_start + b.size;
        // body 头探测, 两种格式 (CN/JP 实测各 277/233):
        //   A: "BKHD"        标准 Wwise bank 头
        //   B: 2E 79 7A 1B   变体头, 28~40B 前导后接明文 DIDX/DATA/HIRC
        // has_bkhd 语义 = "有 body"
        static const uint8_t MAG_B[4] = {0x2E, 0x79, 0x7A, 0x1B};
        uint8_t sig[4];
        if (b.body_start + 8 <= file_size_) {
            _fseeki64(f_, (int64_t)b.body_start, SEEK_SET);
            if (std::fread(sig, 1, 4, f_) == 4 &&
                (std::memcmp(sig, "BKHD", 4) == 0 || std::memcmp(sig, MAG_B, 4) == 0)) {
                b.has_bkhd = true;
            }
        }
        id2idx_[b.id] = i;
    }
    return true;
}

void AkpkFile::close() {
    if (f_) { std::fclose(f_); f_ = nullptr; }
    banks_.clear();
    id2idx_.clear();
}

const BankEntry* AkpkFile::find_bank(uint32_t id) const {
    auto it = id2idx_.find(id);
    return it == id2idx_.end() ? nullptr : &banks_[it->second];
}

bool AkpkFile::read(uint64_t pos, uint32_t size, std::vector<uint8_t>& out) const {
    if (pos + size > file_size_) return false;
    out.resize(size);
    _fseeki64(f_, (int64_t)pos, SEEK_SET);
    return std::fread(out.data(), 1, size, (FILE*)f_) == size;
}

bool AkpkFile::parse_didx(const BankEntry& b, DidxChunk& out) const {
    if (!b.has_bkhd) return false;
    uint64_t pos = b.body_start;
    uint8_t hdr[8];
    {
        std::vector<uint8_t> tmp;
        if (!read(pos, 8, tmp)) return false;
        std::memcpy(hdr, tmp.data(), 8);
    }
    if (std::memcmp(hdr, "BKHD", 4) == 0) {
        pos += 8 + *(uint32_t*)(hdr + 4);
    } else {
        // 格式 B (2E 79 7A 1B): 前导 28~40B 不定, 在头部窗口内扫描 DIDX
        bool found = false;
        for (uint64_t off = 8; off <= 64; off += 4) {
            std::vector<uint8_t> tmp;
            if (!read(b.body_start + off, 4, tmp)) break;
            if (std::memcmp(tmp.data(), "DIDX", 4) == 0) { pos = b.body_start + off; found = true; break; }
        }
        if (!found) return false;
    }

    // 遍历后续 chunk 找 DIDX (最多探 6 个)
    for (int guard = 0; guard < 6; guard++) {
        std::vector<uint8_t> tmp;
        if (!read(pos, 8, tmp)) return false;
        std::memcpy(hdr, tmp.data(), 8);
        uint32_t cs = *(uint32_t*)(hdr + 4);
        if (std::memcmp(hdr, "DIDX", 4) == 0) {
            out.didx_pos = pos;
            out.didx_size = cs;
            out.data_start = pos + 8 + cs + 8;   // DATA chunk 头 8B 之后
            uint32_t cnt = cs / 12;
            std::vector<uint8_t> body(cs);
            if (!read(pos + 8, cs, body)) return false;
            out.media.resize(cnt);
            for (uint32_t k = 0; k < cnt; k++) {
                const uint8_t* e = body.data() + (size_t)k * 12;
                MediaEntry& m = out.media[k];
                m.id = *(uint32_t*)(e + 0);
                uint32_t rel = *(uint32_t*)(e + 4);
                m.size = *(uint32_t*)(e + 8);
                m.abs_pos = out.data_start + rel;
            }
            return true;
        } else if (std::memcmp(hdr, "DATA", 4) == 0 ||
                   std::memcmp(hdr, "HIRC", 4) == 0 ||
                   std::memcmp(hdr, "STMG", 4) == 0) {
            return false;   // DIDX 应在 DATA 之前
        }
        pos += 8 + cs;
    }
    return false;
}

} // namespace nvtool
