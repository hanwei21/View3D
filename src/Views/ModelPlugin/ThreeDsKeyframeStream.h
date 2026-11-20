#pragma once

#include <assimp/DefaultIOSystem.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/types.h>

#include <QtCore/QString>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace View3DModel {

// 3DS 的 keyframer（顶层 chunk 0xB000）里带节点层级。不少非 3ds Max 的导出器
// 不写 0xB040 父子记录（这份 Lib3DS 写的就只有一串 0xB002 节点块），Assimp 于是
// 把每个节点挂到上一个节点下面，层级退化成一条链：实测 223 MB 厂区模型链深
// 16227 层，而 Assimp 沿父链回溯的 InverseNodeSearch 没有深度上限，1 MB 线程栈
// 直接溢出（STATUS_STACK_OVERFLOW，进程连错误框都来不及弹）。三维浏览不需要
// 动画，且同一份文件去掉 keyframer 后包围盒逐字不变，所以按虚拟流把这段摘掉。
struct ThreeDsKeyframeStrip {
    bool applies = false;
    QString note;
    std::uint64_t droppedBytes = 0;
    std::uint64_t virtualSize = 0;
    // 保留下来的顶层 chunk 在源文件里的 (起始, 长度)，顺序不变。虚拟布局是
    // [改写过的 6 字节 MAIN3DS 头] + 这些区间首尾相接。
    std::vector<std::pair<std::uint64_t, std::uint64_t>> segments;
};

// 只看顶层 chunk 链表。声明长度自相矛盾、没有 EDIT3D、或压根没有 keyframer 时
// applies=false，调用方按原样读——宁可不修，也不冒险改坏一份能读的文件。
ThreeDsKeyframeStrip planThreeDsKeyframeStrip(const QString& path);

// 把源文件伪装成一份不含 keyframer 的合法 3DS：Read/Seek/Tell/FileSize 都按虚拟
// 布局走，跨区间的读自动拆分，其余转给底层流。
class ThreeDsKeyframeStream final : public Assimp::IOStream {
  public:
    ThreeDsKeyframeStream(Assimp::IOStream* source, const ThreeDsKeyframeStrip& strip);
    ~ThreeDsKeyframeStream() final;

    std::size_t Read(void* buffer, std::size_t size, std::size_t count) final;
    std::size_t Write(const void* buffer, std::size_t size, std::size_t count) final;
    aiReturn Seek(std::size_t offset, aiOrigin origin) final;
    std::size_t Tell() const final;
    std::size_t FileSize() const final;
    void Flush() final;

  private:
    // 从虚拟位置拷最多 want 个字节，返回实际字节数（0 表示到了虚拟文件末尾）。
    std::size_t copyFromVirtual(char* destination, std::size_t want);

    Assimp::IOStream* m_source;
    ThreeDsKeyframeStrip m_strip;
    std::array<char, 6> m_header {};
    std::size_t m_position = 0;
};

// 只给主文件套虚拟流；材质引用的伴生文件仍走默认实现原样打开。
// DefaultIOSystem 在 6.0.4 里是 final，继承不了，所以用组合转调它。
class ThreeDsKeyframeIOSystem final : public Assimp::IOSystem {
  public:
    ThreeDsKeyframeIOSystem(std::string mainPath, ThreeDsKeyframeStrip strip);

    bool Exists(const char* pFile) const override;
    char getOsSeparator() const override;
    Assimp::IOStream* Open(const char* pFile, const char* pMode = "rb") override;
    void Close(Assimp::IOStream* pFile) override;

  private:
    Assimp::DefaultIOSystem m_backing;
    std::string m_mainPath;
    ThreeDsKeyframeStrip m_strip;
};

} // namespace View3DModel
