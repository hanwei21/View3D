#pragma once

#include "CrsBackend.h"

#include <ICrsLibrary.h>

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace View3DCrs {

// 参照系列表的实现：JSON 库读写 + .prj/.wkt/.json 导入导出 + PROJ 校验。
// 库文件默认落在 <程序目录>/crs_library.json，VIEW3D_CRS_LIBRARY 环境变量覆盖它，
// 无头用例因此能写到自己的临时文件而不碰用户机器上的库。
class CrsLibrary final : public View3D::ICrsLibrary {
  public:
    using Entry = View3D::CrsLibraryEntry;

    explicit CrsLibrary(const CrsBackend& backend);

    // 启动时读入：文件不存在算正常空库；解析不了的条目跳过并在 *error 里报数量。
    bool load(QString* error);
    // 按 id 取一条（契约的 entries() 面向界面，内部改条目要按 id 定位）。
    const Entry* find(const QString& id) const;

    // View3D::ICrsLibrary
    QList<Entry> entries() const override { return m_entries; }
    bool containsDefinition(const QString& definition) const override;
    bool add(const QString& name, const QString& definition, const QString& note,
             Entry* added, QString* error) override;
    bool update(const QString& id, const QString& name, const QString& definition,
                const QString& note, QString* error) override;
    bool remove(const QString& id) override;
    QStringList favorites() const override { return m_favorites; }
    bool isFavorite(const QString& definition) const override;
    void setFavorite(const QString& definition, bool favorite) override;
    QString filePath() const override;
    bool save(QString* error) override;
    bool dirty() const override { return m_dirty; }
    int importFiles(const QStringList& paths, QStringList* failed, QString* error) override;
    bool exportFile(const QString& path, const QStringList& ids, QString* error) override;
    QString lastError() const override { return m_lastError; }

    // 库变了要让 Ribbon 下拉重建选项、让面板刷新列表：回调由插件接到宿主的状态刷新
    // 广播上，面板则订阅「库已就绪」消息后自己重绑。
    void setChangeCallback(std::function<void()> callback);

  private:
    // 导入路径要标记来源，和界面新建区分开，所以内部再多一个带 origin 的版本。
    bool addWithOrigin(const QString& name, const QString& definition, const QString& note,
                       const QString& origin, Entry* added, QString* error);
    // 解析并组装一条条目，但不碰 m_entries：新增走追加、编辑走原地替换，两边共用它。
    bool buildEntry(const QString& name, const QString& definition, const QString& note,
                    const QString& origin, Entry* out, QString* error);
    bool writeJson(const QString& path, const QList<Entry>& entries,
                   const QStringList& favorites, QString* error) const;
    void markChanged();

    // 没有库文件（或老库文件里没收藏）时的默认常用：中国工程最常挑的那几条。
    static QStringList defaultFavorites();

    const CrsBackend& m_backend;
    QList<Entry> m_entries;
    QStringList m_favorites;
    bool m_dirty = false;
    QString m_lastError;
    std::function<void()> m_changed;
};

} // namespace View3DCrs
