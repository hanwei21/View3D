#pragma once

namespace RF {
namespace PluginEngine {

/// Close reason enumeration
enum class CloseReason {
    None = 0,
    WindowsShutDown = 1,
    MdiFormClosing = 2,
    UserClosing = 3,
    TaskManagerClosing = 4,
    FormOwnerClosing = 5,
    ApplicationExitCall = 6
};

class ApplicationClosingEventArgs {
private:
    bool m_cancel;
    CloseReason m_closeReason;

public:
    ApplicationClosingEventArgs()
        : m_cancel(false), m_closeReason(CloseReason::None) {}

    ApplicationClosingEventArgs(bool cancel, CloseReason closeReason)
        : m_cancel(cancel), m_closeReason(closeReason) {}

    bool getCancel() const { return m_cancel; }
    void setCancel(bool cancel) { m_cancel = cancel; }

    CloseReason getCloseReason() const { return m_closeReason; }
};

class ApplicationClosedEventArgs {
private:
    CloseReason m_closeReason;

public:
    ApplicationClosedEventArgs() : m_closeReason(CloseReason::None) {}

    ApplicationClosedEventArgs(CloseReason closeReason)
        : m_closeReason(closeReason) {}

    CloseReason getCloseReason() const { return m_closeReason; }
};

class ContentsViewClosingEventArgs {
private:
    bool m_cancel;
    CloseReason m_closeReason;

public:
    ContentsViewClosingEventArgs()
        : m_cancel(false), m_closeReason(CloseReason::None) {}

    ContentsViewClosingEventArgs(bool cancel, CloseReason closeReason)
        : m_cancel(cancel), m_closeReason(closeReason) {}

    bool getCancel() const { return m_cancel; }
    void setCancel(bool cancel) { m_cancel = cancel; }

    CloseReason getCloseReason() const { return m_closeReason; }
};

class TabbedMdiEventArgs {
private:
    bool m_isSet;
    bool m_tabbedMdi;

public:
    TabbedMdiEventArgs() : m_isSet(false), m_tabbedMdi(false) {}

    TabbedMdiEventArgs(bool tabbedMdi)
        : m_isSet(true), m_tabbedMdi(tabbedMdi) {}

    bool getIsSet() const { return m_isSet; }
    bool getTabbedMdi() const { return m_tabbedMdi; }
    void setTabbedMdi(bool tabbedMdi) { m_tabbedMdi = tabbedMdi; }
};

} // namespace PluginEngine
} // namespace RF
