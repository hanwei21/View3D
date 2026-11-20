#include "ThreeDsKeyframeStream.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

namespace View3DModel {
namespace {

constexpr std::uint16_t kChunkMain3D = 0x4D4D;
constexpr std::uint16_t kChunkEdit3D = 0x3D3D;
constexpr std::uint16_t kChunkKeyframer = 0xB000;
constexpr std::size_t kChunkHeaderBytes = 6;

// Assimp 传回的路径串一般就是我们给它的那个；万一它做了分隔符或大小写规范化，
// 这里也得认出同一个主文件，否则虚拟流套不上去，等于白改。
bool sameNativePath(std::string_view left, std::string_view right) {
    if (left.empty() || left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = static_cast<unsigned char>(left[i] == '\\' ? '/' : left[i]);
        const auto b = static_cast<unsigned char>(right[i] == '\\' ? '/' : right[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

// 3DS 的 chunk 头是小端 2 字节 id + 4 字节长度（长度含这 6 字节）。
std::uint64_t readLittleEndian(QFile& file, std::uint64_t offset, int bytes) {
    if (!file.seek(static_cast<qint64>(offset)))
        return 0;
    const QByteArray raw = file.read(bytes);
    if (raw.size() != bytes)
        return 0;
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i)
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(raw.at(i))) << (8 * i);
    return value;
}

} // namespace

ThreeDsKeyframeStrip planThreeDsKeyframeStrip(const QString& path) {
    ThreeDsKeyframeStrip strip;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        strip.note = QStringLiteral("无法打开源文件，按原样读");
        return strip;
    }
    const std::uint64_t realSize = static_cast<std::uint64_t>(file.size());
    if (realSize < kChunkHeaderBytes * 2 || readLittleEndian(file, 0, 2) != kChunkMain3D) {
        strip.note = QStringLiteral("不是 MAIN3DS 开头，按原样读");
        return strip;
    }
    const std::uint64_t declared = readLittleEndian(file, 2, 4);
    // 顶层链表以实际可读范围为界；声明长度比文件还长时只走声明值，
    // 宁可放弃摘除，也不要把一个自相矛盾的文件改得更矛盾。
    std::uint64_t limit = realSize;
    if (declared >= kChunkHeaderBytes && declared < realSize)
        limit = declared;

    bool sawEdit3D = false;
    bool sawKeyframer = false;
    std::uint64_t keptBytes = 0;
    std::uint64_t offset = kChunkHeaderBytes;
    while (offset + kChunkHeaderBytes <= limit) {
        const std::uint64_t id = readLittleEndian(file, offset, 2);
        const std::uint64_t length = readLittleEndian(file, offset + 2, 4);
        if (length < kChunkHeaderBytes || offset + length > limit)
            break; // 链表在这里断了：后面的字节不再可信
        if (id == kChunkKeyframer) {
            sawKeyframer = true;
            strip.droppedBytes += length;
        } else {
            strip.segments.emplace_back(offset, length);
            keptBytes += length;
            sawEdit3D = sawEdit3D || id == kChunkEdit3D;
        }
        offset += length;
    }

    const auto refuse = [&strip](QString reason) {
        strip.applies = false;
        strip.note = std::move(reason);
        strip.droppedBytes = 0;
        strip.virtualSize = 0;
        strip.segments.clear();
        return strip;
    };
    if (!sawKeyframer)
        return refuse(QStringLiteral("没有 keyframer，按原样读"));
    if (!sawEdit3D)
        return refuse(QStringLiteral("没有 EDIT3D，摘除 keyframer 会读不到几何"));
    if (keptBytes + kChunkHeaderBytes > 0xFFFFFFFFULL)
        return refuse(QStringLiteral("虚拟长度超出 32 位声明字段"));

    strip.virtualSize = keptBytes + kChunkHeaderBytes;
    strip.applies = true;
    return strip;
}

ThreeDsKeyframeStream::ThreeDsKeyframeStream(Assimp::IOStream* source,
                                             const ThreeDsKeyframeStrip& strip)
    : m_source(source), m_strip(strip) {
    // 改写后的头就是"这份文件到 keyframer 之前为止"，Assimp 看到的长度与
    // FileSize() 必须自洽，否则顶层链表校验会直接判文件损坏。
    const auto virtualSize = static_cast<std::uint32_t>(strip.virtualSize);
    m_header = {static_cast<char>(kChunkMain3D & 0xFF),
                static_cast<char>(kChunkMain3D >> 8),
                static_cast<char>(virtualSize & 0xFF),
                static_cast<char>((virtualSize >> 8) & 0xFF),
                static_cast<char>((virtualSize >> 16) & 0xFF),
                static_cast<char>((virtualSize >> 24) & 0xFF)};
}

ThreeDsKeyframeStream::~ThreeDsKeyframeStream() {
    delete m_source;
}

std::size_t ThreeDsKeyframeStream::copyFromVirtual(char* destination, std::size_t want) {
    if (m_position >= m_strip.virtualSize || want == 0)
        return 0;
    if (m_position < kChunkHeaderBytes) {
        const std::size_t taken = std::min(want, kChunkHeaderBytes - m_position);
        std::memcpy(destination, m_header.data() + m_position, taken);
        m_position += taken;
        return taken;
    }
    std::size_t cursor = kChunkHeaderBytes;
    for (const auto& [start, length] : m_strip.segments) {
        if (m_position < cursor + length) {
            const std::size_t inside = static_cast<std::size_t>(m_position - cursor);
            const std::size_t available = static_cast<std::size_t>(length) - inside;
            const std::size_t wantHere = std::min(want, available);
            if (m_source->Seek(static_cast<std::size_t>(start) + inside, aiOrigin_SET) !=
                aiReturn_SUCCESS)
                return 0;
            const std::size_t taken = m_source->Read(destination, 1, wantHere);
            m_position += taken;
            return taken;
        }
        cursor += length;
    }
    return 0;
}

std::size_t ThreeDsKeyframeStream::Read(void* buffer, std::size_t size, std::size_t count) {
    if (size == 0 || count == 0)
        return 0;
    char* destination = static_cast<char*>(buffer);
    const std::size_t bytes = size * count;
    std::size_t copied = 0;
    while (copied < bytes) {
        const std::size_t taken = copyFromVirtual(destination + copied, bytes - copied);
        if (taken == 0)
            break; // 与 fread 一致：短读即到达末尾
        copied += taken;
    }
    return copied / size;
}

std::size_t ThreeDsKeyframeStream::Write(const void*, std::size_t, std::size_t) {
    return 0; // 只读视图
}

aiReturn ThreeDsKeyframeStream::Seek(std::size_t offset, aiOrigin origin) {
    if (origin == aiOrigin_SET)
        m_position = offset;
    else if (origin == aiOrigin_CUR)
        m_position += offset;
    else if (origin == aiOrigin_END)
        m_position = static_cast<std::size_t>(m_strip.virtualSize) + offset; // 允许像 fseek 一样传负数
    else
        return aiReturn_FAILURE;
    return aiReturn_SUCCESS;
}

std::size_t ThreeDsKeyframeStream::Tell() const {
    return m_position;
}

std::size_t ThreeDsKeyframeStream::FileSize() const {
    return static_cast<std::size_t>(m_strip.virtualSize);
}

void ThreeDsKeyframeStream::Flush() {}

ThreeDsKeyframeIOSystem::ThreeDsKeyframeIOSystem(std::string mainPath,
                                                 ThreeDsKeyframeStrip strip)
    : m_mainPath(std::move(mainPath)), m_strip(std::move(strip)) {}

bool ThreeDsKeyframeIOSystem::Exists(const char* pFile) const {
    return m_backing.Exists(pFile);
}

char ThreeDsKeyframeIOSystem::getOsSeparator() const {
    return m_backing.getOsSeparator();
}

Assimp::IOStream* ThreeDsKeyframeIOSystem::Open(const char* pFile, const char* pMode) {
    Assimp::IOStream* source = m_backing.Open(pFile, pMode);
    if (!source)
        return nullptr;
    const bool isMainFile = pFile && sameNativePath(m_mainPath, std::string_view(pFile));
    const bool readOnly = pMode && pMode[0] == 'r';
    // 认路径不认次序：Assimp 在真正解析前会先开一次文件做格式探测（BaseImporter::Check），
    // 只包装第一次打开的话，解析那一趟拿到的还是带 keyframer 的原始字节，等于没改。
    if (!isMainFile || !readOnly || !m_strip.applies)
        return source;
    return new ThreeDsKeyframeStream(source, m_strip);
}

void ThreeDsKeyframeIOSystem::Close(Assimp::IOStream* pFile) {
    // 包装流的析构会回收它接管的底层流，所以这里交给默认实现 delete 就够了。
    m_backing.Close(pFile);
}

} // namespace View3DModel
