#pragma once
#include <QString>
#include <functional>
namespace RF::PluginEngine {
struct CommandState { bool enabled=true; bool checked=false; bool visible=true; };
class ICommandStatePublisher {
public:
 using Listener=std::function<void(const QString&,const CommandState&)>;
 virtual ~ICommandStatePublisher()=default;
 virtual CommandState commandState(const QString&)const=0;
 virtual int subscribeCommandState(Listener)=0;
 virtual void unsubscribeCommandState(int)=0;
};}
