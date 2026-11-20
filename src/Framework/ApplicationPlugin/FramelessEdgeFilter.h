#pragma once

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QWidget>

namespace View3DApplication {

// SARibbon 内置的 SAFramelessHelper 把窗口边缘 5px×DPR 当作缩放带，但它的过滤器挂在
// 主窗口上，只看得见投递给主窗口自身（以及它代为转发的 ribbonBar）的事件。右上角的
// 系统按钮、ribbon 右侧命令按钮、停靠区这些子控件会把落在缩放带上的按下直接吞掉：
// 贴右上角抓边框既不缩放，按钮还顺带被点着（最大化/关闭）。这里把边缘带内的鼠标事件
// 改投给主窗口走助手的缩放分支，并且不再交给原控件。
// 同一套取边判定也用来吞掉缩放带内的左键双击：助手判"双击标题栏最大化"时只比较 Y 与
// 标题栏高度，不看这次按下是否在缩放带上，而 Windows 会把双击间隔内的第二次按下报成
// MouseButtonDblClick、不再发 Press，于是贴边反复抓框就被当成双击标题栏而最大化。
// 装在应用级过滤器上：notify 先问应用过滤器，再问对象自己的过滤器，因此必定抢在助手前。
class FramelessEdgeFilter final : public QObject
{
  public:
    explicit FramelessEdgeFilter(QWidget *window) : QObject(window), m_window(window)
    {
        if (m_window)
            QApplication::instance()->installEventFilter(this);
    }

    // 与 SAFramelessHelper 的取边判定同式：5px 基准边宽 × 屏幕 DPR，向下取整。
    static int edgeBandWidth(const QWidget *window)
    {
        if (!window)
            return 0;
        const QScreen *screen = window->screen();
        const qreal dpiScale = screen ? screen->devicePixelRatio() : 1.0;
        return static_cast<int>(kBorderWidth * dpiScale);
    }

    static bool isOnEdgeBand(const QWidget *window, const QPoint &globalPos)
    {
        const int band = edgeBandWidth(window);
        if (!window || band <= 0)
            return false;
        const QRect frame = window->frameGeometry();
        const int left = frame.x();
        const int top = frame.y();
        const int right = left + frame.width();
        const int bottom = top + frame.height();
        return (globalPos.x() >= left && globalPos.x() <= left + band)
            || (globalPos.x() >= right - band && globalPos.x() <= right)
            || (globalPos.y() >= top && globalPos.y() <= top + band)
            || (globalPos.y() >= bottom - band && globalPos.y() <= bottom);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
        case QEvent::MouseButtonDblClick:
            break;
        default:
            return false;
        }
        if (!m_window)
            return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (watched == m_window)
            return swallowEdgeDoubleClick(mouse);
        QWidget *target = qobject_cast<QWidget *>(watched);
        if (target && m_window->isAncestorOf(target))
            return takeOverEdgeMouseEvent(mouse);
        return false;
    }

  private:
    bool swallowEdgeDoubleClick(QMouseEvent *mouse)
    {
        if (mouse->type() != QEvent::MouseButtonDblClick || mouse->button() != Qt::LeftButton
            || m_window->isMaximized() || m_window->isFullScreen()
            || !isOnEdgeBand(m_window, mouse->globalPosition().toPoint()))
            return false;
        mouse->accept();
        return true;
    }

    // 缩放带内的左键按下由主窗口的助手接管；接管期间后续移动/释放一律转投，因为鼠标
    // 早就离开那条窄带了，剩下的只有按住不放。
    bool takeOverEdgeMouseEvent(QMouseEvent *mouse)
    {
        switch (mouse->type()) {
        case QEvent::MouseButtonPress:
            if (m_edgeTakenOver)
                break;
            if (mouse->button() != Qt::LeftButton || m_window->isMaximized()
                || m_window->isFullScreen()
                || !isOnEdgeBand(m_window, mouse->globalPosition().toPoint()))
                return false;
            m_edgeTakenOver = true;
            break;
        case QEvent::MouseMove:
            if (!m_edgeTakenOver)
                return false;
            // 按下被弹窗之类抢走时别把接管状态留成死结。
            if (mouse->buttons() == Qt::NoButton) {
                m_edgeTakenOver = false;
                return false;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (!m_edgeTakenOver)
                return false;
            m_edgeTakenOver = false;
            break;
        case QEvent::MouseButtonDblClick:
            // 带内的二次按下：交给主窗口，由 swallowEdgeDoubleClick 吞掉，控件不再收到点击。
            if (!isOnEdgeBand(m_window, mouse->globalPosition().toPoint()))
                return false;
            break;
        default:
            return false;
        }
        QApplication::sendEvent(m_window, mouse);
        mouse->accept();
        return true;
    }

    static constexpr int kBorderWidth = 5;
    QWidget *m_window = nullptr;
    bool m_edgeTakenOver = false;
};

} // namespace View3DApplication
