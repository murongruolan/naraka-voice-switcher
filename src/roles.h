// roles.h — 角色 ID 表 + FNV-1 (bank 命名: dialogue_ingame_<id> 等)
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace nvtool {

struct RoleInfo {
    const char* en;   // bank 命名用 id (小写)
    const char* cn;   // 中文名
};

// 24 个已确认角色 (官方英文名 FNV-1 对撞验证 2026-11-09)
extern const std::vector<RoleInfo> ROLE_TABLE;

// 4 类 bank 前缀
extern const char* const BANK_PREFIXES[4];

uint32_t fnv1_str(const std::string& s);

} // namespace nvtool
