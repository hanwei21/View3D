#pragma once

#include <QString>

// 把 std::cerr / std::clog 同时写一份到文件。GUI 程序双击启动时没有控制台，
// 换机器排查倾斜摄影这类问题时，现场只剩 Ogre 自己那份日志，所以插件打印的
// VIEW3D_* 诊断必须落盘。启动时截断，只保留本次会话的内容。
void installLogTee(const QString& path);
