#pragma once

#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>

#include <QHash>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace View3DTiles {

// 3D Tiles 只读本地文件系统。URL 一律相对于 tileset.json 所在目录，
// 因此外部 tileset / 瓦片 / glTF 外部贴图都能按规范里的相对路径解析。
// 3D Tiles 1.1 的 multiple contents 没有被 Cesium Native 0.62 直接支持，
// 这里在读到清单时把 `contents` 合成为虚拟 cmpt，加载器看到的就是 1.0 形式。
class TilesetAssetAccessor final
    : public CesiumAsync::IAssetAccessor,
      public std::enable_shared_from_this<TilesetAssetAccessor> {
  public:
    explicit TilesetAssetAccessor(const QString& rootDirectory);

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> get(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url,
        const std::vector<THeader>& headers) override;
    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& method,
        const std::string& url, const std::vector<THeader>& headers,
        const std::span<const std::byte>& contentPayload) override;
    void tick() noexcept override;

    std::uint64_t requestCount() const noexcept { return m_requests.load(); }
    std::uint64_t missingCount() const noexcept { return m_missing.load(); }
    std::uint64_t multipleContentsCount() const noexcept
    {
        return m_multipleContents.load();
    }
    QStringList warnings() const;

    const QString& rootDirectory() const noexcept { return m_root; }

  private:
    std::shared_ptr<CesiumAsync::IAssetRequest> load(const std::string& url);
    std::shared_ptr<CesiumAsync::IAssetRequest> failureRequest(const std::string& url);
    // 返回 false 表示这份 JSON 不是可归一化的 1.1 清单，原样返回。
    bool normalizeMultipleContents(const QString& manifestKey, std::vector<std::byte>* data);
    std::vector<std::byte> buildComposite(const QStringList& sourceKeys);
    QString resolveRelative(const QString& url, bool* withinRoot) const;
    void warn(const QString& message) const;

    QString m_root;
    mutable std::mutex m_virtualMutex;
    mutable std::mutex m_warningMutex;
    QHash<QString, QStringList> m_virtualContents;
    mutable QStringList m_warnings;
    std::atomic<std::uint64_t> m_requests { 0 };
    std::atomic<std::uint64_t> m_missing { 0 };
    std::atomic<std::uint64_t> m_multipleContents { 0 };
    std::atomic<std::uint32_t> m_virtualSerial { 0 };
};

} // namespace View3DTiles
