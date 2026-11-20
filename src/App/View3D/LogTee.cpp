#include "LogTee.h"

#include <QDir>
#include <QFileInfo>

#include <fstream>
#include <iostream>
#include <memory>
#include <streambuf>

namespace {

// 原样透传给控制台那一侧，同时写进文件；std::cerr 本身已在处理行缓冲，
// 这里只在 sync() 时刷文件，避免每字符一次 IO。
class TeeStreamBuf final : public std::streambuf {
  public:
    TeeStreamBuf(std::streambuf* console, std::ostream* file)
        : m_console(console), m_file(file)
    {
    }

  protected:
    int overflow(int character) override
    {
        if (character == EOF)
            return traits_type::not_eof(character);
        const char value = static_cast<char>(character);
        if (m_console)
            m_console->sputc(value);
        if (m_file)
            m_file->put(value);
        return character;
    }

    int sync() override
    {
        if (m_console)
            m_console->pubsync();
        if (m_file)
            m_file->flush();
        return 0;
    }

  private:
    std::streambuf* m_console = nullptr;
    std::ostream* m_file = nullptr;
};

} // namespace

void installLogTee(const QString& path)
{
    // 静态持有：streambuf 必须活到进程结束，否则 std::cerr 会指向悬空对象。
    static std::ofstream file;
    static std::unique_ptr<TeeStreamBuf> buffer;
    if (buffer)
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    file.open(path.toStdString(), std::ios::out | std::ios::trunc);
    if (!file.is_open())
        return;
    buffer = std::make_unique<TeeStreamBuf>(std::cerr.rdbuf(), &file);
    std::cerr.rdbuf(buffer.get());
    std::clog.rdbuf(buffer.get());
}
