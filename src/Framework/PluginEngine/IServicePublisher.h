#pragma once
#include <typeinfo>

#include <QList>
#include <QString>

namespace RF::PluginEngine {

// 控件向服务注册表贡献的服务：对象指针 + 发布时登记的静态类型。
// 类型必须是指针想要被解析成的那个接口类型，注册表据此校验 objectAs<T>()。
struct PublishedService {
    QString name;
    void *object = nullptr;
    const std::type_info *type = nullptr;
};

class IServicePublisher {
public:
    using PublishedService = RF::PluginEngine::PublishedService;
    virtual ~IServicePublisher() = default;
    virtual QList<PublishedService> publishedServices() const = 0;
};

} // namespace RF::PluginEngine
