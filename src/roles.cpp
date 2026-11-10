// roles.cpp — 角色表实现
#include "roles.h"
#include "akpk.h"

namespace nvtool {

const std::vector<RoleInfo> ROLE_TABLE = {
    {"viper",      "宁红夜"},
    {"feria",      "沈妙"},
    {"tenkai",     "天海"},
    {"zipingyin",  "殷紫萍"},
    {"temulch",    "特木尔"},
    {"tarka",      "季沧海"},
    {"kurumi",     "胡桃"},
    {"yotohime",   "妖刀姬"},
    {"valda",      "崔三娘"},
    {"yueshan",    "岳山"},
    {"wuchen",     "无尘"},
    {"justinagu",  "顾清寒"},
    {"takeda",     "武田"},
    {"matari",     "迦南"},
    {"akos",       "胡为"},
    {"zai",        "季莹莹"},
    {"tessa",      "玉玲珑"},
    {"hadi",       "哈迪"},
    {"shayol",     "魏轻"},
    {"lyam",       "刘炼"},
    {"kylin",      "张起灵"},
    {"cyra",       "席拉"},
    {"lannie",     "蓝梦"},
    {"wanjun",     "万钧"},
    {"lixunhuan",  "李寻欢"},
    {"wuzhen",     "巫真"},
    {"tara",       "甘璇"},
    {"yexiu",      "叶修"},
    {"nangongjin", "南宫锦"},
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
