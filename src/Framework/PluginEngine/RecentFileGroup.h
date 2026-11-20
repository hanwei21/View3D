#pragma once

#include <QString>
#include <vector>
#include "IRecentFileGroup.h"

namespace RF {
namespace PluginEngine {

class RecentFileGroup : public IRecentFileGroup {
private:
    struct RecentFileItem {
        QString filePath;
        bool checked;
        RecentFileItem(const QString& fp, bool chk) : filePath(fp), checked(chk) {}
    };

    std::vector<RecentFileItem> m_recentFiles;
    QString m_key;
    QString m_title;
    int m_maxCount;

public:
    RecentFileGroup(const QString& key, const QString& title, int maxCount)
        : m_key(key), m_title(title), m_maxCount(maxCount) {}

    bool clear() override {
        m_recentFiles.clear();
        return true;
    }

    bool insert(int index, const QString& filePath) override {
        bool isCheck = false;
        for (auto it = m_recentFiles.begin(); it != m_recentFiles.end(); ++it) {
            if (it->filePath == filePath) {
                isCheck = it->checked;
                m_recentFiles.erase(it);
                break;
            }
        }
        int count = static_cast<int>(m_recentFiles.size()) - 1;
        while (static_cast<int>(m_recentFiles.size()) >= m_maxCount && count >= 0) {
            if (!m_recentFiles[count].checked) {
                m_recentFiles.erase(m_recentFiles.begin() + count);
            }
            count--;
        }
        if (static_cast<int>(m_recentFiles.size()) >= m_maxCount)
            return false;
        m_recentFiles.insert(m_recentFiles.begin() + index, RecentFileItem(filePath, isCheck));
        return true;
    }

    bool remove(const QString& filePath) override {
        for (auto it = m_recentFiles.begin(); it != m_recentFiles.end(); ++it) {
            if (it->filePath == filePath) {
                m_recentFiles.erase(it);
                return true;
            }
        }
        return false;
    }

    bool removeAt(int index) override {
        if (index < 0 || index >= static_cast<int>(m_recentFiles.size())) return false;
        m_recentFiles.erase(m_recentFiles.begin() + index);
        return true;
    }

    bool removeRange(int startIndex, int count) override {
        if (startIndex < 0 || startIndex + count > static_cast<int>(m_recentFiles.size())) return false;
        m_recentFiles.erase(m_recentFiles.begin() + startIndex, m_recentFiles.begin() + startIndex + count);
        return true;
    }

    bool setChecked(const QString& filePath, bool isChecked) override {
        for (auto& item : m_recentFiles) {
            if (item.filePath == filePath) {
                item.checked = isChecked;
                return true;
            }
        }
        return false;
    }

    bool getChecked(const QString& filePath) override {
        for (auto& item : m_recentFiles) {
            if (item.filePath == filePath)
                return item.checked;
        }
        return false;
    }

    int getMaxCount() const override { return m_maxCount; }
    int getCount() const override { return static_cast<int>(m_recentFiles.size()); }

    QString getItem(int index) const override {
        if (index >= 0 && index < static_cast<int>(m_recentFiles.size()))
            return m_recentFiles[index].filePath;
        return QString();
    }

    QString getTitle() const override { return m_title; }
    QString getKey() const override { return m_key; }

    void notify(const QString& filePath) {
        if (RecentFileClick.hasHandlers())
            RecentFileClick(filePath);
    }
};

} // namespace PluginEngine
} // namespace RF
