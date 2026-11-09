// roles.cpp — 角色表实现
#include "roles.h"
#include "akpk.h"

namespace nvtool {

const std::vector<RoleInfo> ROLE_TABLE = {
    {"lannie",     "蓝梦"},
    {"kurumi",     "胡桃"},
    {"zai",        "季莹莹"},
    {"yotohime",   "妖刀姬"},
    {"tessa",      "玉玲珑"},
    {"feria",      "沈妙"},
    {"zipingyin",  "殷紫萍"},
    {"justinagu",  "顾清寒"},
    {"shayol",     "魏轻"},
    {"viper",      "宁红夜"},
    {"tarka",      "季沧海"},
    {"valda",      "崔三娘"},
    {"akos",       "胡为"},
    {"matari",     "迦南"},
    {"temulch",    "特木尔"},
    {"hadi",       "哈迪"},
    {"takeda",     "武田"},
    {"wuchen",     "无尘"},
    {"yueshan",    "岳山"},
    {"tenkai",     "天海"},
    {"lixunhuan",  "李寻欢"},
    {"wanjun",     "万钧"},
    {"wuzhen",     "巫真"},
    {"cyra",       "席拉"},
    {"kylin",      "张起灵"},
    {"lyam",       "刘炼"},
    {"nangongjin", "南宫锦"},
    {"yexiu",      "叶修"},
    {"tara",       "甘璇"},
    {"yunqing",    "云庆"},
};

const char* const BANK_PREFIXES[4] = {
    "dialogue_ingame_", "dialogue_outgame_", "dialogue_marker_", "voice_"
};

uint32_t fnv1_str(const std::string& s) {
    // 统一小写后 FNV-1 (bank 命名规律验证过)
    std::string low = s;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return AkpkFile::fnv1(low);
}

} // namespace nvtool
