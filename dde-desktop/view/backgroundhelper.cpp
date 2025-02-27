/*
 * Copyright (C) 2017 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "backgroundhelper.h"
#include "util/xcb/xcb.h"

#include <QScreen>
#include <QGuiApplication>
#include <QBackingStore>
#include <QPainter>
#include <QPaintEvent>
#include <qpa/qplatformwindow.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformbackingstore.h>
#define private public
#include <private/qhighdpiscaling_p.h>
#undef private

class BackgroundLabel : public QWidget
{
public:
    using QWidget::QWidget;

    void setPixmap(const QPixmap &pixmap)
    {
        m_pixmap = pixmap;
        m_noScalePixmap = pixmap;
        m_noScalePixmap.setDevicePixelRatio(1);
        update();
    }

    void paintEvent(QPaintEvent *event) override
    {
        qreal scale = devicePixelRatioF();

        if (scale > 1.0 && event->rect() == rect()) {
            if (backingStore()->handle()->paintDevice()->devType() != QInternal::Image) {
                return;
            }

            QImage *image = static_cast<QImage*>(backingStore()->handle()->paintDevice());
            QPainter pa(image);
            pa.drawPixmap(0, 0, m_noScalePixmap);
        }

        QPainter pa(this);
        pa.drawPixmap(event->rect().topLeft(), m_pixmap, QRect(event->rect().topLeft() * scale, event->rect().size() * scale));
    }

    virtual void setVisible(bool visible) override {
        if (!visible) {
            // 暂时（紧急）解决arm64双屏切换复制模式容易出现无法显示桌面的问题，禁止隐藏任何桌面。
            // 后续有较好的解决方案可以删除此代码
            qDebug() << "not allow to hide desktop(screen is " << property("myScreen").toString() <<
                       ") primaryScreen is " << qApp->primaryScreen()->name();
            return ;
        }
        QWidget::setVisible(visible);
    }

private:
    QPixmap m_pixmap;
    QPixmap m_noScalePixmap;
};

BackgroundHelper *BackgroundHelper::desktop_instance = nullptr;

BackgroundHelper::BackgroundHelper(bool preview, QObject *parent)
    : QObject(parent)
    , m_previuew(preview)
    , windowManagerHelper(DWindowManagerHelper::instance())
{
    if (!preview) {
        connect(windowManagerHelper, &DWindowManagerHelper::windowManagerChanged,
                this, &BackgroundHelper::onWMChanged);
        connect(windowManagerHelper, &DWindowManagerHelper::hasCompositeChanged,
                this, &BackgroundHelper::onWMChanged);
        desktop_instance = this;
    }

    checkTimer = new QTimer(this);
    checkTimer->setInterval(2000);
    checkTimer->setSingleShot(true);
    connect(checkTimer, &QTimer::timeout, this, [this](){
        checkBlackScreen();
    });

    onWMChanged();
}

BackgroundHelper::~BackgroundHelper()
{
    for (BackgroundLabel *l : backgroundMap) {
        l->hide();
        l->deleteLater();
    }
}

BackgroundHelper* BackgroundHelper::getDesktopInstance()
{
    return desktop_instance;
}

bool BackgroundHelper::isEnabled() const
{
    // 只支持kwin，或未开启混成的桌面环境
    return windowManagerHelper->windowManagerName() == DWindowManagerHelper::KWinWM || !windowManagerHelper->hasComposite();
}

QWidget *BackgroundHelper::backgroundForScreen(QScreen *screen) const
{
    return backgroundMap.value(screen);
}

QList<QWidget *> BackgroundHelper::allBackgrounds() const
{
    QList<QWidget*> backgrounds;

    for (QWidget *w : backgroundMap)
        backgrounds << w;

    return backgrounds;
}

void BackgroundHelper::setBackground(const QString &path)
{
    qInfo() << "path:" << path;
    currentWallpaper = path.startsWith("file:") ? QUrl(path).toLocalFile() : path;
    backgroundPixmap = QPixmap(currentWallpaper);

    // 更新背景图
    for (BackgroundLabel *l : backgroundMap) {
        updateBackground(l);
    }
}

void BackgroundHelper::setVisible(bool visible)
{
    m_visible = visible;

    for (BackgroundLabel *l : backgroundMap) {
        l->setVisible(visible);
    }
}

bool BackgroundHelper::isKWin() const
{
    return windowManagerHelper->windowManagerName() == DWindowManagerHelper::KWinWM;
}

bool BackgroundHelper::isDeepinWM() const
{
    return windowManagerHelper->windowManagerName() == DWindowManagerHelper::DeepinWM;
}

static bool wmDBusIsValid()
{
    return QDBusConnection::sessionBus().interface()->isServiceRegistered("com.deepin.wm");
}

void BackgroundHelper::onWMChanged()
{
    if (m_previuew || isEnabled()) {
        if (wmInter) {
            return;
        }

        wmInter = new WMInter("com.deepin.wm", "/com/deepin/wm", QDBusConnection::sessionBus(), this);
        gsettings = new DGioSettings("com.deepin.dde.appearance", "", this);

        if (!m_previuew) {
            connect(wmInter, &WMInter::WorkspaceSwitched, this, [this] (int, int to) {
                currentWorkspaceIndex = to;
                updateBackground();
            });

            connect(gsettings, &DGioSettings::valueChanged, this, [this] (const QString &key, const QVariant &value) {
                Q_UNUSED(value);
                if (key == "background-uris") {
                    updateBackground();
                }
            });
        }

        connect(qApp, &QGuiApplication::screenAdded, this, &BackgroundHelper::onScreenAdded);
        connect(qApp, &QGuiApplication::screenRemoved, this, &BackgroundHelper::onScreenRemoved);

        // 初始化窗口
        for (QScreen *s : qApp->screens()) {
            onScreenAdded(s);
        }

        // 初始化背景图
        updateBackground();
    } else {
        if (!wmInter) {
            return;
        }

        // 清理数据
        gsettings->deleteLater();
        gsettings = nullptr;

        wmInter->deleteLater();
        wmInter = nullptr;

        currentWallpaper.clear();
        currentWorkspaceIndex = 0;
        backgroundPixmap = QPixmap();

        disconnect(qApp, &QGuiApplication::screenAdded, this, &BackgroundHelper::onScreenAdded);
        disconnect(qApp, &QGuiApplication::screenRemoved, this, &BackgroundHelper::onScreenRemoved);

        // 销毁窗口
        for (QScreen *s : backgroundMap.keys()) {
            onScreenRemoved(s);
        }
    }

    Q_EMIT enableChanged();
}

void BackgroundHelper::updateBackground(QWidget *l)
{
    if (backgroundPixmap.isNull())
        return;

    QScreen *s = l->windowHandle()->screen();
    l->windowHandle()->handle()->setGeometry(s->handle()->geometry());

    const QSize trueSize = s->handle()->geometry().size();
    QPixmap pix = backgroundPixmap;

    pix = pix.scaled(trueSize,
                     Qt::KeepAspectRatioByExpanding,
                     Qt::SmoothTransformation);

    if (pix.width() > trueSize.width() || pix.height() > trueSize.height()) {
        pix = pix.copy(QRect((pix.width() - trueSize.width()) / 2.0,
                             (pix.height() - trueSize.height()) / 2.0,
                             trueSize.width(),
                             trueSize.height()));
    }

    pix.setDevicePixelRatio(l->devicePixelRatioF());
    dynamic_cast<BackgroundLabel*>(l)->setPixmap(pix);

    qInfo() << l->windowHandle()->screen() << currentWallpaper << pix;

    if (checkTimer) {
        checkTimer->start();
    }
}

void BackgroundHelper::updateBackground()
{
    QString path = wmDBusIsValid() ? wmInter->GetCurrentWorkspaceBackground() : QString();

    if (path.isEmpty() || !QFile::exists(QUrl(path).toLocalFile())
            // 调用失败时会返回 "The name com.deepin.wm was not provided by any .service files"
            // 此时 wmInter->isValid() = true, 且 dubs last error type 为 NoError
            || (!path.startsWith("/") && !path.startsWith("file:"))) {
        path = gsettings->value("background-uris").toStringList().value(currentWorkspaceIndex);

        if (path.isEmpty()) {
            qWarning() << "invalid path, will not setbackground";
            return;
        }
    }

    setBackground(path);
}

void BackgroundHelper::onScreenAdded(QScreen *screen)
{
    BackgroundLabel *l = new BackgroundLabel();
    l->setProperty("myScreen",screen->name()); // assert screen->name is unique

    backgroundMap[screen] = l;

    l->createWinId();
    l->windowHandle()->setScreen(screen);
    l->setGeometry(screen->geometry());

    QTimer::singleShot(0, l, [l, screen] {
        // 禁用高分屏缩放，防止窗口的sizeIncrement默认设置大于1
        bool hi_active = QHighDpiScaling::m_active;
        QHighDpiScaling::m_active = false;
        l->windowHandle()->handle()->setGeometry(screen->handle()->geometry());
        QHighDpiScaling::m_active = hi_active;
    });

    if (m_previuew) {
        l->setWindowFlags(l->windowFlags() | Qt::BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus);
    } else {
        Xcb::XcbMisc::instance().set_window_type(l->winId(), Xcb::XcbMisc::Desktop);
    }

    if (m_visible)
        l->show();
    else
        qDebug() << "Disable show the background widget, of screen:" << screen << screen->geometry();

    connect(screen, &QScreen::geometryChanged, l, [l, this, screen] () {
        qDebug() << "screen geometry changed:" << screen << screen->geometry();

        updateBackgroundGeometry(screen, l);

        Q_EMIT backgroundGeometryChanged(l);
    });

    // 可能是由QGuiApplication引发的新屏幕添加，此处应该为新对象添加背景图
    updateBackground(l);

    Q_EMIT backgroundGeometryChanged(l);
    Q_EMIT backgroundAdded(l);

    qInfo() << screen << screen->geometry();
}

void BackgroundHelper::onScreenRemoved(QScreen *screen)
{
    if (BackgroundLabel *l = backgroundMap.take(screen)) {
        Q_EMIT aboutDestoryBackground(l);

        l->deleteLater();
    }

    qInfo() << screen;
}

void BackgroundHelper::updateBackgroundGeometry(QScreen *screen, BackgroundLabel *l)
{
    // 因为接下来会发出backgroundGeometryChanged信号，
    // 所以此处必须保证QWidget::geometry的值和接下来对其windowHandle()对象设置的geometry一致
    l->setGeometry(screen->geometry());

    // 忽略屏幕缩放，设置窗口的原始大小
    // 调用此函数后不会立即更新QWidget::geometry，而是在收到窗口resize事件后更新
    bool hi_active = QHighDpiScaling::m_active;
    QHighDpiScaling::m_active = false;
    l->windowHandle()->handle()->setGeometry(screen->handle()->geometry());
    QHighDpiScaling::m_active = hi_active;
    updateBackground(l);
}

void BackgroundHelper::checkBlackScreen()
{
    qDebug() << "check it out";

    QScreen *ps = qApp->primaryScreen();
    QWidget *psl = backgroundForScreen(ps);
    QList<QWidget *> ls = allBackgrounds();
    QList<QScreen *> ss = qApp->screens();
    QSet<QScreen *> myScreens;

    for (QWidget *l : ls) {
        myScreens << l->windowHandle()->screen();
    }

    bool hasBlackScreen = myScreens.size()<ss.size();
    bool isPrimaryScreenBlack = false;
    if (!hasBlackScreen) {
         //qDebug() << "No black Screen Found...";
        return;
    } else {
        QSet<QScreen *>blackScreens = ss.toSet() - myScreens;
        qInfo() << "primaryScreen is " << ps->name();

        for (QScreen *s : blackScreens) {
            qInfo() << "black Screen Found..." << s->name();
            if (!isPrimaryScreenBlack) {
                isPrimaryScreenBlack = s == ps;
            }
        }
    }

    if (isPrimaryScreenBlack && psl) {
        qInfo() << "try restore primary Screen " << ps->name() << psl->property("myScreen");
        psl->windowHandle()->setScreen(ps);
        updateBackgroundGeometry(ps, static_cast<BackgroundLabel *>(psl));
    }

    // remove primaryScreen's label from ls
    for (int i=0; i<ls.size(); ++i) {
        if (ls.value(i)==psl) {
            qInfo() << "remove " << ls[i]->property("myScreen");
            ls.removeAt(i);
            break;
        }
    }

    // remove primaryScreen  from ss
    for (int i=0; i<ss.size(); ++i) {
        if (ss.value(i)==ps) {
            qInfo() << "remove " << ss[i]->name();
            ss.removeAt(i);
            break;
        }
    }

    Q_ASSERT(ls.size() == ss.size());
    for (int i=0; i<ls.size() && i<ss.size(); ++i) {
        if (ls[i]->windowHandle()->screen()!=ss[i]) {
            ls[i]->windowHandle()->setScreen(ss[i]);
        }
        updateBackgroundGeometry(ss[i], static_cast<BackgroundLabel *>(ls[i]));
    }
}

void BackgroundHelper::resetBackgroundVisibleState()
{
    for (QScreen * screen : qGuiApp->screens()) {
        BackgroundLabel * l = backgroundMap.value(screen);
        l->show();
    }
}
#ifdef QT_DEBUG
void BackgroundHelper::printLog()
{
    qDebug() << "\n************************\n";
    for (QScreen * screen : qGuiApp->screens()) {
        BackgroundLabel * l = backgroundMap.value(screen);
        qDebug() << screen->name() << "\n"<<
                 "handle->geometry" <<screen->handle()->geometry() <<"\n"<<
                  "\r\n----------------------\r\n" <<
                  "label.screen" << l->windowHandle()->handle()->screen()->name()<<
                  "label geo" << l->windowHandle()->handle()->geometry();
    }
    qDebug() << "\n************************\n" <<
                "backgroundMap.size() " << backgroundMap.size() <<
                "\n************************\n";
     for (BackgroundLabel *l : backgroundMap) {
         qDebug() << "\r\n" << l << "l->isvisible" << l->isVisible() <<
                     "property.myScreen" << l->property("myScreen") <<
                     "\r\nlabel.screen" << l->windowHandle()->handle()->screen()->name()<<
                     "label geo" << l->windowHandle()->handle()->geometry();

     }
     qDebug() << "\n************************\n";
}

void BackgroundHelper::printLog(int index)
{
     QList<QWidget*> backgrounds = allBackgrounds();
     QWidget *l = backgrounds.value(index);
     if (l) {
         qDebug() << l << "\nl->isvisible" << l->isVisible() <<
                     "property.myScreen" << l->property("myScreen") <<
                     "label.screen" << l->windowHandle()->handle()->screen()->name()<<
                     "label geo" << l->windowHandle()->handle()->geometry();
     } else {
         qWarning() << "invalid index" << "backgrounds.size" << backgrounds.size();
     }
}

void BackgroundHelper::mapLabelScreen(int labelIndex, int screenIndex)
{
    QList<QWidget*> backgrounds = allBackgrounds();
    QWidget *l = backgrounds.value(labelIndex);
    if (!l) {
        qWarning() << "invalid index" << "backgrounds.size" << backgrounds.size();
        return;
    }
    QScreen *screen = qGuiApp->screens().value(screenIndex);
    if (!screen) {
        qWarning() << "invalid index" << "screens.size" << qGuiApp->screens().size();
        return;
    }

    if (l->windowHandle()->screen()!=screen) {
        l->windowHandle()->setScreen(screen);
    }

    updateBackgroundGeometry(screen, static_cast<BackgroundLabel *>(l));
}
#endif //QT_DEBUG
