// gui.h — M3 本地 GUI 服务
#pragma once

namespace nvtool {
// 启动本地 HTTP 服务并自动打开浏览器, 阻塞直至服务退出
int run_gui();
} // namespace nvtool
