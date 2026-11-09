// rebuild.cpp — pck 重建引擎实现
#include "rebuild.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>

namespace nvtool {

// ---- 内部: body chunk 布局 ----
struct BodyChunks {
    uint64_t bkhd_pos = 0;
    uint32_t bkhd_csize = 0;
    uint64_t didx_pos = 0;
    uint32_t didx_size = 0;
    uint64_t data_pos = 0;      // DATA chunk 头位置
    uint32_t data_decl = 0;     // DATA 声明大小
    uint64_t data_bytes = 0;    // 数据起点
    uint64_t tail_start = 0;    // DATA 数据结束 (尾部 chunk/填充起点)
    bool ok = false;
};

static bool scan_body(AkpkFile& f, const BankEntry& b, BodyChunks& c) {
    if (!b.has_bkhd) return false;
    uint64_t pos = b.body_start;
    std::vector<uint8_t> h8;
    if (!f.read(pos, 8, h8)) return false;
    if (std::memcmp(h8.data(), "BKHD", 4) != 0) return false;
    c.bkhd_pos = pos;
    c.bkhd_csize = *(uint32_t*)(h8.data() + 4);
    pos += 8 + c.bkhd_csize;
    if (!f.read(pos, 8, h8)) return false;
    if (std::memcmp(h8.data(), "DIDX", 4) != 0) return false;
    c.didx_pos = pos;
    c.didx_size = *(uint32_t*)(h8.data() + 4);
    pos += 8 + c.didx_size;
    if (!f.read(pos, 8, h8)) return false;
    if (std::memcmp(h8.data(), "DATA", 4) != 0) return false;
    c.data_pos = pos;
    c.data_decl = *(uint32_t*)(h8.data() + 4);
    c.data_bytes = pos + 8;
    c.tail_start = c.data_bytes + c.data_decl;
    if (c.tail_start > b.body_start + b.size) {
        // DATA 声明越界 -> 结构异常
        return false;
    }
    c.ok = true;
    return true;
}

// DIDX entries: (id, off, size)
struct Med { uint32_t id, off, size; };
static std::vector<Med> read_didx(AkpkFile& f, const BodyChunks& c) {
    std::vector<Med> out;
    std::vector<uint8_t> buf(c.didx_size);
    if (f.read(c.didx_pos + 8, c.didx_size, buf)) {
        size_t cnt = c.didx_size / 12;
        out.resize(cnt);
        for (size_t k = 0; k < cnt; k++) {
            out[k].id  = *(uint32_t*)(buf.data() + k*12 + 0);
            out[k].off = *(uint32_t*)(buf.data() + k*12 + 4);
            out[k].size = *(uint32_t*)(buf.data() + k*12 + 8);
        }
    }
    return out;
}

// 按 offset 排序的索引序列
static std::vector<size_t> order_by_offset(const std::vector<Med>& e) {
    std::vector<size_t> idx(e.size());
    for (size_t i = 0; i < e.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(),
              [&](size_t a, size_t b){ return e[a].off < e[b].off; });
    return idx;
}

// ---- SHA-256 (精简实现) ----
namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t len = 0;
    uint8_t buf[64]; size_t blen = 0;
    static uint32_t rotr(uint32_t x, int n){ return (x>>n)|(x<<(32-n)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f2=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch = (e&f2)^((~e)&g);
            uint32_t t1 = hh+S1+ch+K[i]+w[i];
            uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0+maj;
            hh=g; g=f2; f2=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f2; h[6]+=g; h[7]+=hh;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = (64 - blen) < n ? (64 - blen) : n;
            std::memcpy(buf + blen, p, take);
            blen += take; p += take; n -= take;
            if (blen == 64) { block(buf); blen = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (blen != 56) update(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - i*8));
        update(l, 8);
        char out[65];
        for (int i = 0; i < 8; i++)
            std::sprintf(out + i*8, "%08x", h[i]);
        return std::string(out, 64);
    }
};
} // namespace

std::string sha256_file(const std::wstring& path) {
    FILE* fp = nullptr;
#if defined(_WIN32)
    fp = _wfopen(path.c_str(), L"rb");
#else
    fp = std::fopen(std::string(path.begin(), path.end()).c_str(), "rb");
#endif
    if (!fp) return "";
    Sha256 s;
    std::vector<uint8_t> buf(1 << 20);   // 堆分配: 工作线程栈仅 1MB, 栈上 1MB 缓冲会溢出
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), fp)) > 0) s.update(buf.data(), n);
    std::fclose(fp);
    return s.hex();
}

// ---- 主重建 ----
bool rebuild_pck(const std::wstring& cn_path, const std::wstring& jp_path,
                 const std::wstring& out_path, RebuildStats& st,
                 const std::function<void(uint64_t done, uint64_t total)>& progress,
                 const std::vector<uint32_t>& only_banks) {
    AkpkFile cn, jp;
    if (!cn.open(cn_path)) { st.log.push_back("打开中文包失败"); return false; }
    if (!jp.open(jp_path)) { st.log.push_back("打开日语包失败"); return false; }

    st.banks_total = (int)cn.banks().size();
    FILE* out = nullptr;
#if defined(_WIN32)
    out = _wfopen(out_path.c_str(), L"wb");
#else
    out = std::fopen(std::string(out_path.begin(), out_path.end()).c_str(), "wb");
#endif
    if (!out) { st.log.push_back("输出文件创建失败"); return false; }

    // 1) 头区: 原样 [0, data_start) —— bank 表 size 字段稍后回写
    std::vector<uint8_t> header;
    cn.read(0, cn.data_start(), header);
    // 动态定位表 (与 AkpkFile::open 相同逻辑):
    uint64_t ent_pos = 0; uint32_t cnt = 0;
    for (uint64_t c = 0x0C; c + 4 <= cn.data_start() && c < 0x400; c += 4) {
        uint64_t rem = cn.data_start() - c;
        if (rem == 0 || rem % 20) continue;
        uint32_t n = (uint32_t)(rem / 20);
        if (n < 100 || n > 100000) continue;
        std::vector<uint8_t> t4;
        if (!cn.read(c - 4, 4, t4)) continue;
        if (*(uint32_t*)t4.data() != n) continue;
        uint32_t v0 = 0;
        std::vector<uint8_t> t1;
        if (!cn.read(c, 4, t1)) continue;
        v0 = *(uint32_t*)t1.data();
        if (v0) { ent_pos = c; cnt = n; break; }
    }
    if (!cnt) { st.log.push_back("中文包 bank 表定位失败"); std::fclose(out); return false; }

    // 2) 逐 bank: JP 有 body -> 整体用 JP body (HIRC 自洽, 语义 100% 正确)
    //          JP 无 body -> CN body 原样
    struct NewBody { std::vector<uint8_t> bytes; };
    std::vector<NewBody> bodies(cn.banks().size());
    uint64_t acc = cn.data_start();

    for (size_t i = 0; i < cn.banks().size(); i++) {
        const BankEntry& b = cn.banks()[i];
        uint64_t start = AkpkFile::align_up(acc, 4096);
        acc = start + b.size;

        std::vector<uint8_t> cn_raw;
        const bool cn_ok = cn.read(b.body_start, b.size, cn_raw);

        // 角色过滤: only_banks 非空时仅替换命中 id (按角色部分替换)
        const bool want = only_banks.empty() ||
                          std::find(only_banks.begin(), only_banks.end(), b.id) != only_banks.end();

        if (b.has_bkhd && want) {
            const BankEntry* jb = jp.find_bank(b.id);
            if (jb && jb->has_bkhd && jb->size > 0) {
                std::vector<uint8_t> raw;
                if (jp.read(jb->body_start, jb->size, raw)) {
                    // 语言 ID 补回 CN 原值: body+16 (格式A=BKHD内容+8, 格式B=前导dword4)
                    // 该值 = fnv1(语言名), 如 chinese(cn)=f1efa9da; JP 值会被游戏校验拒载 -> 全包静音
                    if (cn_ok && cn_raw.size() >= 20 && raw.size() >= 20) {
                        std::memcpy(raw.data() + 16, cn_raw.data() + 16, 4);
                    }
                    bodies[i].bytes = std::move(raw);
                    st.banks_replaced++;
                    st.media_replaced += jb->size;   // 换为 body 字节数口径
                    continue;
                }
            }
            st.banks_kept_mismatch++;
            st.log.push_back("bank " + std::to_string(i) + " JP 无可用 body -> 保留中文");
        } else {
            st.banks_kept_nobody++;
        }
        bodies[i].bytes = std::move(cn_raw);
    }

    // 3) 回写 bank 表: size(+8) + 扇区号(+12)
    //    +12 字段 = body 物理位置 >> 12 (游戏按 A<<12 定位 body, v2/v3 没回写导致全包静音)
    //    +4 恒 0x1000 (对齐常量), +16 恒 1 (标志位), 均沿用 CN 原值
    {
        uint64_t acc2 = cn.data_start();
        for (size_t i = 0; i < bodies.size(); i++) {
            uint64_t pos = AkpkFile::align_up(acc2, 4096);
            acc2 = pos + bodies[i].bytes.size();
            uint8_t* e = header.data() + ent_pos + i*20;
            uint32_t nsz  = (uint32_t)bodies[i].bytes.size();
            uint32_t nsec = (uint32_t)(pos >> 12);
            for (int t = 0; t < 4; t++) {
                e[8+t]  = (uint8_t)(nsz >> (t*8));
                e[12+t] = (uint8_t)(nsec >> (t*8));
            }
        }
    }

    // 4) 写出: 头区 + 数据区(4096 对齐), 进度回调按字节推进
    uint64_t total_est = header.size();
    for (auto& nb : bodies) {
        uint64_t pad = AkpkFile::align_up(total_est, 4096) - total_est;
        total_est += pad + nb.bytes.size();
    }
    if (progress) progress(0, total_est);

    std::fwrite(header.data(), 1, header.size(), out);
    st.bytes_written += header.size();
    uint64_t accw = header.size();
    if (progress) progress(accw, total_est);
    for (auto& nb : bodies) {
        uint64_t pad = AkpkFile::align_up(accw, 4096) - accw;
        static const uint8_t zero[4096] = {0};
        if (pad) { std::fwrite(zero, 1, pad, out); accw += pad; st.bytes_written += pad; }
        std::fwrite(nb.bytes.data(), 1, nb.bytes.size(), out);
        accw += nb.bytes.size();
        st.bytes_written += nb.bytes.size();
        if (progress) progress(accw, total_est);
    }
    std::fclose(out);
    return true;
}

} // namespace nvtool
