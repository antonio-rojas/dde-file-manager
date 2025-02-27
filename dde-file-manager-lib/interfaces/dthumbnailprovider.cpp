/*
 * Copyright (C) 2016 ~ 2018 Deepin Technology Co., Ltd.
 *               2016 ~ 2018 dragondjf
 *
 * Author:     dragondjf<dingjiangfeng@deepin.com>
 *
 * Maintainer: dragondjf<dingjiangfeng@deepin.com>
 *             zccrs<zhangjide@deepin.com>
 *             Tangtong<tangtong@deepin.com>
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

#include "dthumbnailprovider.h"
#include "dfmstandardpaths.h"
#include "dmimedatabase.h"
#include "shutil/fileutils.h"
#include "app/define.h"
#include "singleton.h"
#include "shutil/mimetypedisplaymanager.h"
#include "fileoperations/filejob.h"
#include "dfmapplication.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QImageReader>
#include <QQueue>
#include <QMimeType>
#include <QReadWriteLock>
#include <QWaitCondition>
#include <QPainter>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QDebug>

// use original poppler api
#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page.h>
#include <poppler-page-renderer.h>

#include <DThumbnailProvider>

DFM_BEGIN_NAMESPACE

#define FORMAT ".png"

inline QByteArray dataToMd5Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

class DThumbnailProviderPrivate
{
public:
    DThumbnailProviderPrivate(DThumbnailProvider *qq);

    void init();

    QString sizeToFilePath(DThumbnailProvider::Size size) const;

    DThumbnailProvider *q_ptr;
    QString errorString;
    // 5MB
    qint64 defaultSizeLimit = 1024 * 1024 * 20;
    QHash<QMimeType, qint64> sizeLimitHash;
    DMimeDatabase mimeDatabase;

    static QSet<QString> hasThumbnailMimeHash;

    struct ProduceInfo {
        QFileInfo fileInfo;
        DThumbnailProvider::Size size;
        DThumbnailProvider::CallBack callback;
    };

    QQueue<ProduceInfo> produceQueue;
    QSet<QPair<QString, DThumbnailProvider::Size>> discardedProduceInfos;

    bool running = true;

    QWaitCondition waitCondition;
    QReadWriteLock dataReadWriteLock;

    QHash<QString, QString> keyToThumbnailTool;

    Q_DECLARE_PUBLIC(DThumbnailProvider)
};

QSet<QString> DThumbnailProviderPrivate::hasThumbnailMimeHash;

DThumbnailProviderPrivate::DThumbnailProviderPrivate(DThumbnailProvider *qq)
    : q_ptr(qq)
{

}

void DThumbnailProviderPrivate::init()
{
    sizeLimitHash.reserve(28);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("text/plain"), 1024 * 1024);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("application/pdf"), INT64_MAX);
//    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("application/vnd.adobe.flash.movie"), INT64_MAX);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("application/vnd.rn-realmedia"), INT64_MAX);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("application/vnd.ms-asf"), INT64_MAX);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("application/mxf"), INT64_MAX);

    //images
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("image/ief"), 1024 * 1024 * 80);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("image/tiff"), 1024 * 1024 * 80);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("image/x-tiff-multipage"), 1024 * 1024 * 80);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("image/x-adobe-dng"), 1024 * 1024 * 80);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("image/jpeg"), 1024 * 1024 * 30);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("image/png"), 1024 * 1024 * 30);
    sizeLimitHash.insert(mimeDatabase.mimeTypeForName("image/pipeg"), 1024 * 1024 * 30);
}

QString DThumbnailProviderPrivate::sizeToFilePath(DThumbnailProvider::Size size) const
{
    switch (size) {
    case DThumbnailProvider::Small:
        return DFMStandardPaths::location(DFMStandardPaths::ThumbnailSmallPath);
    case DThumbnailProvider::Normal:
        return DFMStandardPaths::location(DFMStandardPaths::ThumbnailNormalPath);
    case DThumbnailProvider::Large:
        return DFMStandardPaths::location(DFMStandardPaths::ThumbnailLargePath);
    }

    return QString();
}

class DFileThumbnailProviderPrivate : public DThumbnailProvider {};
Q_GLOBAL_STATIC(DFileThumbnailProviderPrivate, ftpGlobal)

DThumbnailProvider *DThumbnailProvider::instance()
{
    return ftpGlobal;
}

bool DThumbnailProvider::hasThumbnail(const QFileInfo &info) const
{
    Q_D(const DThumbnailProvider);

    if (!info.isReadable() || !info.isFile())
        return false;

    qint64 fileSize = info.size();

    if (fileSize <= 0)
        return false;

    const QMimeType &mime = d->mimeDatabase.mimeTypeForFile(info);

    if (mime.name().startsWith("video/") && FileJob::CopyingFiles.contains(DUrl::fromLocalFile(info.filePath())))
        return false;

    if (mime.name().startsWith("video/") && FileUtils::isGvfsMountFile(info.absoluteFilePath()))
        return false;

    if (fileSize > sizeLimit(mime) && !mime.name().startsWith("video/"))
        return false;

    return hasThumbnail(mime);
}

bool DThumbnailProvider::hasThumbnail(const QMimeType &mimeType) const
{
    const QString &mime = mimeType.name();
    QStringList mimeTypeList = {mime};
    mimeTypeList.append(mimeType.parentMimeTypes());

    if (mime.startsWith("image") && !DFMApplication::instance()->genericAttribute(DFMApplication::GA_PreviewImage).toBool())
        return false;

    if ((mime.startsWith("video") || mimeTypeDisplayManager->supportVideoMimeTypes().contains(mime))
            && !DFMApplication::instance()->genericAttribute(DFMApplication::GA_PreviewVideo).toBool())
        return false;

    if (mime == "text/plain" && !DFMApplication::instance()->genericAttribute(DFMApplication::GA_PreviewTextFile).toBool())
        return false;

    if (Q_LIKELY(mimeTypeList.contains("application/pdf")
                 || mime == "application/cnd.rn-realmedia"
                 || mime == "application/mxf")
            && !DFMApplication::instance()->genericAttribute(DFMApplication::GA_PreviewDocumentFile).toBool()) {
        return false;
    }

    if (DThumbnailProviderPrivate::hasThumbnailMimeHash.contains(mime))
        return true;

    if (Q_LIKELY(mime.startsWith("image") || mime.startsWith("video/"))) {
        DThumbnailProviderPrivate::hasThumbnailMimeHash.insert(mime);

        return true;
    }

    if (Q_LIKELY(mime == "text/plain" || mimeTypeList.contains("application/pdf")
//            || mime == "application/vnd.adobe.flash.movie"
            || mime == "application/vnd.rn-realmedia"
            || mime == "application/vnd.ms-asf"
            || mime == "application/mxf")) {
        DThumbnailProviderPrivate::hasThumbnailMimeHash.insert(mime);

        return true;
    }

    if (DTK_GUI_NAMESPACE::DThumbnailProvider::instance()->hasThumbnail(mimeType))
        return true;

    return false;
}

QString DThumbnailProvider::thumbnailFilePath(const QFileInfo &info, Size size) const
{
    Q_D(const DThumbnailProvider);

    const QString &absolutePath = info.absolutePath();
    const QString &absoluteFilePath = info.absoluteFilePath();

    if (absolutePath == d->sizeToFilePath(Small)
            || absolutePath == d->sizeToFilePath(Normal)
            || absolutePath == d->sizeToFilePath(Large)
            || absolutePath == DFMStandardPaths::location(DFMStandardPaths::ThumbnailFailPath)) {
        return absoluteFilePath;
    }

    const QString thumbnailName = dataToMd5Hex(QUrl::fromLocalFile(absoluteFilePath).toString(QUrl::FullyEncoded).toLocal8Bit()) + FORMAT;
    QString thumbnail = d->sizeToFilePath(size) + QDir::separator() + thumbnailName;

    if (!QFile::exists(thumbnail)) {
        return QString();
    }

    QImageReader ir(thumbnail, QByteArray(FORMAT).mid(1));

    ir.setAutoDetectImageFormat(false);

    const QImage image = ir.read();

    if (!image.isNull() && image.text(QT_STRINGIFY(Thumb::MTime)).toInt() != (int)info.lastModified().toTime_t()) {
        QFile::remove(thumbnail);

        emit thumbnailChanged(absoluteFilePath, QString());

        return QString();
    }

    return thumbnail;
}

static QString generalKey(const QString &key)
{
    const QStringList &_tmp = key.split('/');

    if (_tmp.size() > 1)
        return _tmp.first() + "/*";

    return key;
}

QString DThumbnailProvider::createThumbnail(const QFileInfo &info, DThumbnailProvider::Size size)
{
    Q_D(DThumbnailProvider);

    d->errorString.clear();

    const QString &absolutePath = info.absolutePath();
    const QString &absoluteFilePath = info.absoluteFilePath();

    if (absolutePath == d->sizeToFilePath(Small)
            || absolutePath == d->sizeToFilePath(Normal)
            || absolutePath == d->sizeToFilePath(Large)
            || absolutePath == DFMStandardPaths::location(DFMStandardPaths::ThumbnailFailPath)) {
        return absoluteFilePath;
    }

    if (!hasThumbnail(info)) {
        d->errorString = QStringLiteral("This file has not support thumbnail: ") + absoluteFilePath;

        //!Warnning: Do not store thumbnails to the fail path
        return QString();
    }

    const QString fileUrl = QUrl::fromLocalFile(absoluteFilePath).toString(QUrl::FullyEncoded);
    const QString thumbnailName = dataToMd5Hex(fileUrl.toLocal8Bit()) + FORMAT;

    // the file is in fail path
    QString thumbnail = DFMStandardPaths::location(DFMStandardPaths::ThumbnailFailPath) + QDir::separator() + thumbnailName;

//    if (QFile::exists(thumbnail)) {
//        QImage image(thumbnail);

//        if (image.text(QT_STRINGIFY(Thumb::MTime)).toInt() != (int)info.lastModified().toTime_t()) {
//            QFile::remove(thumbnail);
//        } else {
//            return QString();
//        }
//    }// end

    QMimeType mime = d->mimeDatabase.mimeTypeForFile(info);
    QScopedPointer<QImage> image(new QImage());

    QStringList mimeTypeList = {mime.name()};
    mimeTypeList.append(mime.parentMimeTypes());

    if (mime.name().startsWith("image/")) {
        mime = d->mimeDatabase.mimeTypeForFile(info, QMimeDatabase::MatchContent);

        QImageReader reader(absoluteFilePath, mime.preferredSuffix().toLatin1());

        if (!reader.canRead()) {
            d->errorString = reader.errorString();
            goto _return;
        }

        const QSize &imageSize = reader.size();

//        if(!imageSize.isValid()){
//            d->errorString = "Fail to read image file attribute data:" + info.absoluteFilePath();
//            goto _return;
//        }

        if (imageSize.width() > size || imageSize.height() > size || mime.name() == "image/svg+xml") {
            reader.setScaledSize(reader.size().scaled(size, size, Qt::KeepAspectRatio));
        }

        if (!reader.read(image.data())) {
            d->errorString = reader.errorString();
            goto _return;
        }

        if (image->width() > size || image->height() > size) {
            image->operator =(image->scaled(size, size, Qt::KeepAspectRatio));
        }
    } else if (mime.name() == "text/plain") {
        //FIXME(zccrs): This should be done using the image plugin?
        QFile file(absoluteFilePath);

        if (!file.open(QIODevice::ReadOnly)) {
            d->errorString = file.errorString();
            goto _return;
        }

        QString text{ DFMGlobal::toUnicode(file.read(2000), file.fileName()) };
        file.close();

        QFont font;
        font.setPixelSize(12);

        QPen pen;
        pen.setColor(Qt::black);

        *image = QImage(0.70707070 * size, size, QImage::Format_ARGB32_Premultiplied);
        image->fill(Qt::white);

        QPainter painter(image.data());
        painter.setFont(font);
        painter.setPen(pen);

        QTextOption option;

        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        painter.drawText(image->rect(), text, option);
    } else if (mimeTypeList.contains("application/pdf")) {
        //FIXME(zccrs): This should be done using the image plugin?
        QScopedPointer<poppler::document> doc(poppler::document::load_from_file(absoluteFilePath.toStdString()));

        if (!doc || doc->is_locked()) {
            d->errorString = QStringLiteral("Cannot read this pdf file: ") + absoluteFilePath;
            goto _return;
        }

        if (doc->pages() < 1) {
            d->errorString = QStringLiteral("This stream is invalid");
            goto _return;
        }

        QScopedPointer<const poppler::page> page(doc->create_page(0));

        if (!page) {
            d->errorString = QStringLiteral("Cannot get this page at index 0");
            goto _return;
        }

        poppler::page_renderer pr;
        pr.set_render_hint(poppler::page_renderer::antialiasing, true);
        pr.set_render_hint(poppler::page_renderer::text_antialiasing, true);

        poppler::image imageData = pr.render_page(page.data(), 72, 72, -1, -1, -1, size);

        if (!imageData.is_valid()) {
            d->errorString = QStringLiteral("Render error");
            goto _return;
        }

        poppler::image::format_enum format = imageData.format();
        QImage img;

        switch (format) {
        case poppler::image::format_invalid:
            d->errorString = QStringLiteral("Image format is invalid");
            goto _return;
        case poppler::image::format_mono:
            img = QImage((uchar*)imageData.data(), imageData.width(), imageData.height(), QImage::Format_Mono);
            break;
        case poppler::image::format_rgb24:
            img = QImage((uchar*)imageData.data(),imageData.width(),imageData.height(),QImage::Format_ARGB6666_Premultiplied);
            break;
        case poppler::image::format_argb32:
            img = QImage((uchar*)imageData.data(), imageData.width(), imageData.height(), QImage::Format_ARGB32);
            break;
        default:
            break;
        }

        if (img.isNull()) {
            d->errorString = QStringLiteral("Render error");
            goto _return;
        }

        *image = img.scaled(QSize(size, size), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        thumbnail = DTK_GUI_NAMESPACE::DThumbnailProvider::instance()->createThumbnail(info, (DTK_GUI_NAMESPACE::DThumbnailProvider::Size)size);
        d->errorString = DTK_GUI_NAMESPACE::DThumbnailProvider::instance()->errorString();

        if (d->errorString.isEmpty()) {
            emit createThumbnailFinished(absoluteFilePath, thumbnail);
            emit thumbnailChanged(absoluteFilePath, thumbnail);

            return thumbnail;
        } else { // fallback to thumbnail tool
            if (d->keyToThumbnailTool.isEmpty()) {
                d->keyToThumbnailTool["Initialized"] = QString();

                for (const QString &path : QString(TOOLDIR).split(":")) {
                    const QString &thumbnail_tool_path = path + QDir::separator() + "/thumbnail";
                    QDirIterator dir(thumbnail_tool_path, {"*.json"}, QDir::NoDotAndDotDot | QDir::Files);

                    while (dir.hasNext()) {
                        const QString &file_path = dir.next();
                        const QFileInfo &file_info = dir.fileInfo();

                        QFile file(file_path);

                        if (!file.open(QFile::ReadOnly)) {
                            continue;
                        }

                        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
                        file.close();

                        const QStringList keys = document.object().toVariantMap().value("Keys").toStringList();
                        const QString &tool_file_path = file_info.absoluteDir().filePath(file_info.baseName());

                        if (!QFile::exists(tool_file_path)) {
                            continue;
                        }

                        for (const QString &key : keys) {
                            if (d->keyToThumbnailTool.contains(key))
                                continue;

                            d->keyToThumbnailTool[key] = tool_file_path;
                        }
                    }
                }
            }

            QString mime_name = mime.name();
            QString tool = d->keyToThumbnailTool.value(mime_name);

            if (tool.isEmpty()) {
                mime_name = generalKey(mime_name);
                tool = d->keyToThumbnailTool.value(mime_name);
            }

            if (tool.isEmpty()) {
                return thumbnail;
            }

            QProcess process;
            process.start(tool, {QString::number(size), absoluteFilePath}, QIODevice::ReadOnly);

            if (!process.waitForFinished()) {
                d->errorString = process.errorString();

                goto _return;
            }

            if (process.exitCode() != 0) {
                const QString &error = process.readAllStandardError();

                if (error.isEmpty()) {
                    d->errorString = QString("get thumbnail failed from the \"%1\" application").arg(tool);
                } else {
                    d->errorString = error;
                }

                goto _return;
            }

            const QByteArray png_data = QByteArray::fromBase64(process.readAllStandardOutput());
            Q_ASSERT(!png_data.isEmpty());

            if (image->loadFromData(png_data, "png")) {
                d->errorString.clear();
            } else {
                d->errorString = QString("load png image failed from the \"%1\" application").arg(tool);
            }
        }
    }

_return:
    // successful
    if (d->errorString.isEmpty()) {
        thumbnail = d->sizeToFilePath(size) + QDir::separator() + thumbnailName;
    } else {
        //fail
        image.reset(new QImage(1, 1, QImage::Format_Mono));
    }

    image->setText(QT_STRINGIFY(Thumb::URL), fileUrl);
    image->setText(QT_STRINGIFY(Thumb::MTime), QString::number(info.lastModified().toTime_t()));

    // create path
    QFileInfo(thumbnail).absoluteDir().mkpath(".");

    if (!image->save(thumbnail, Q_NULLPTR, 80)) {
        d->errorString = QStringLiteral("Can not save image to ") + thumbnail;
    }

    if (d->errorString.isEmpty()) {
        emit createThumbnailFinished(absoluteFilePath, thumbnail);
        emit thumbnailChanged(absoluteFilePath, thumbnail);

        return thumbnail;
    }

    // fail
    emit createThumbnailFailed(absoluteFilePath);

    return QString();
}

void DThumbnailProvider::appendToProduceQueue(const QFileInfo &info, DThumbnailProvider::Size size, DThumbnailProvider::CallBack callback)
{
    DThumbnailProviderPrivate::ProduceInfo produceInfo;

    produceInfo.fileInfo = info;
    produceInfo.size = size;
    produceInfo.callback = callback;

    Q_D(DThumbnailProvider);

    if (isRunning()) {
        QWriteLocker locker(&d->dataReadWriteLock);
        d->produceQueue.append(std::move(produceInfo));
        locker.unlock();
        d->waitCondition.wakeAll();
    } else {
        d->produceQueue.append(std::move(produceInfo));
        start();
    }
}

void DThumbnailProvider::removeInProduceQueue(const QFileInfo &info, DThumbnailProvider::Size size)
{
    Q_D(DThumbnailProvider);

    if (isRunning()) {
        QWriteLocker locker(&d->dataReadWriteLock);
        Q_UNUSED(locker)
    }

    d->discardedProduceInfos.insert(qMakePair(info.absoluteFilePath(), size));
}

QString DThumbnailProvider::errorString() const
{
    Q_D(const DThumbnailProvider);

    return d->errorString;
}

qint64 DThumbnailProvider::defaultSizeLimit() const
{
    Q_D(const DThumbnailProvider);

    return d->defaultSizeLimit;
}

void DThumbnailProvider::setDefaultSizeLimit(qint64 size)
{
    Q_D(DThumbnailProvider);

    d->defaultSizeLimit = size;
}

qint64 DThumbnailProvider::sizeLimit(const QMimeType &mimeType) const
{
    Q_D(const DThumbnailProvider);

    return d->sizeLimitHash.value(mimeType, d->defaultSizeLimit);
}

void DThumbnailProvider::setSizeLimit(const QMimeType &mimeType, qint64 size)
{
    Q_D(DThumbnailProvider);

    d->sizeLimitHash[mimeType] = size;
}

DThumbnailProvider::DThumbnailProvider(QObject *parent)
    : QThread(parent)
    , d_ptr(new DThumbnailProviderPrivate(this))
{
    d_func()->init();
}

DThumbnailProvider::~DThumbnailProvider()
{
    Q_D(DThumbnailProvider);

    d->running = false;
    d->waitCondition.wakeAll();
    wait();
}

void DThumbnailProvider::run()
{
    Q_D(DThumbnailProvider);

    forever {
        QWriteLocker locker(&d->dataReadWriteLock);

        if (d->produceQueue.isEmpty()) {
            d->waitCondition.wait(&d->dataReadWriteLock);
        }

        if (!d->running)
            return;

        const DThumbnailProviderPrivate::ProduceInfo &task = d->produceQueue.dequeue();
        const QPair<QString, DThumbnailProvider::Size> &tmpKey = qMakePair(task.fileInfo.absoluteFilePath(), task.size);

        if (d->discardedProduceInfos.contains(tmpKey)) {
            d->discardedProduceInfos.remove(tmpKey);
            locker.unlock();
            continue;
        }

        locker.unlock();

        const QString &thumbnail = createThumbnail(task.fileInfo, task.size);

        if (task.callback)
            task.callback(thumbnail);
    }
}

DFM_END_NAMESPACE
