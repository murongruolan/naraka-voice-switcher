// rebuild.h — pck 替换重建引擎
// 对齐策略 (M2 实测结论):
//   1. bank 对齐按 bank_id (中日同名 bank 510/510 全命中)
//   2. 媒体对齐按 "offset 排序后的顺序" (两包 media id 体系独立, id 不可用)
//   3. 仅处理两侧媒体数量相等的 bank; 数量不等或无 body -> 原样保留中文
//   4. DIDX 保留中文 media id (游戏事件引用 CN id), DATA 换 JP 字节
//   5. 头区 bank 表 size 重写; A 字段不变 (媒体数不变的 bank 不影响全局索引)
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include "akpk.h"

namespace nvtool {

struct RebuildStats {
    int banks_total = 0;
    int banks_replaced = 0;   // 成功替换
    int banks_kept_mismatch = 0;  // 媒体数不等, 保留中文
    int banks_kept_nobody = 0;    // 无 body, 原样
    int banks_kept_error = 0;     // 结构异常, 保留中文
    uint64_t media_replaced = 0;
    uint64_t bytes_written = 0;
    std::vector<std::string> log;
};

// 主入口: 读中文包 + 日语包 -> 输出新 pck 到 out_path
// 只做文件读写, 不做备份/进程检测 (那是上层职责)
// progress: 可选, 写盘进度回调 (done/total 字节), 供 GUI 进度条
// only_banks: 可选, 非空时仅替换这些 bank id (按角色部分替换), 其余保留中文 body
bool rebuild_pck(const std::wstring& cn_path,
                 const std::wstring& jp_path,
                 const std::wstring& out_path,
                 RebuildStats& st,
                 const std::function<void(uint64_t done, uint64_t total)>& progress = {},
                 const std::vector<uint32_t>& only_banks = {});

// 精简 SHA-256 (无依赖)
std::string sha256_file(const std::wstring& path);

} // namespace nvtool
