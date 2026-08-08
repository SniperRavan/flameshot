// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#include "flameshot.h"
#include "core/flameshotdaemon.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#include "qhotkey.h"
#endif

#if defined(Q_OS_MACOS)
#include <QWindow>
#include <objc/message.h>

namespace {

constexpr long NSApplicationActivationPolicyRegular = 0;
constexpr long NSApplicationActivationPolicyAccessory = 1;

void setActivationPolicy(long policy)
{
    auto sharedApp = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    auto setPolicy = reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend);
    id nsApp = sharedApp(reinterpret_cast<id>(objc_getClass("NSApplication")),
                         sel_registerName("sharedApplication"));
    setPolicy(nsApp, sel_registerName("setActivationPolicy:"), policy);
}

void setActivationPolicyRegular()
{
    setActivationPolicy(NSApplicationActivationPolicyRegular);
}

void setActivationPolicyAccessory()
{
    setActivationPolicy(NSApplicationActivationPolicyAccessory);
}

constexpr const char* visibleInDockProperty = "_visibleInDock";

} // namespace

#include <CoreGraphics/CoreGraphics.h>
#endif

#include "config/configresolver.h"
#include "config/configwindow.h"
#include "core/qguiappcurrentscreen.h"
#include "utils/abstractlogger.h"
#include "utils/confighandler.h"
#include "utils/screengrabber.h"
#include "utils/screenshotsaver.h"
#include "widgets/capture/capturewidget.h"
#include "widgets/capturelauncher.h"
#include "widgets/infowindow.h"

#ifdef ENABLE_IMGUR
#include "tools/imgupload/imguploadermanager.h"
#include "tools/imgupload/storages/imguploaderbase.h"
#include "widgets/imguploaddialog.h"
#include "widgets/uploadhistory.h"
#endif

#ifdef ENABLE_OCR
#include <tesseract/baseapi.h>
#endif

#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QMessageBox>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#if defined(Q_OS_MACOS)
#include <QScreen>
#endif

Flameshot::Flameshot()
  : m_haveExternalWidget(false)
  , m_captureWindow(nullptr)
#if (defined(Q_OS_MACOS) || defined(Q_OS_WIN))
  , m_HotkeyScreenshotCapture(nullptr)
#endif
#if (defined(Q_OS_MACOS) && ENABLE_IMGUR)
  , m_HotkeyScreenshotHistory(nullptr)
#endif
{
    QString StyleSheet = CaptureButton::globalStyleSheet();
    qApp->setStyleSheet(StyleSheet);

#if defined(Q_OS_MACOS)
    // Request Screen Recording permission via the proper CoreGraphics API
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
    }
#endif
#if (defined(Q_OS_MACOS) || defined(Q_OS_WIN))
    // Set global shortcuts for MacOS or Windows
    m_HotkeyScreenshotCapture = new QHotkey(
      QKeySequence(ConfigHandler().shortcut("TAKE_SCREENSHOT")), true, this);
    QObject::connect(m_HotkeyScreenshotCapture,
                     &QHotkey::activated,
                     qApp,
                     [this]() { gui(); });
#endif
#if (defined(Q_OS_MACOS) && ENABLE_IMGUR)
    m_HotkeyScreenshotHistory = new QHotkey(
      QKeySequence(ConfigHandler().shortcut("SCREENSHOT_HISTORY")), true, this);
    QObject::connect(m_HotkeyScreenshotHistory,
                     &QHotkey::activated,
                     qApp,
                     [this]() { history(); });
#endif
}

Flameshot* Flameshot::instance()
{
    static Flameshot c;
    return &c;
}

CaptureWidget* Flameshot::gui(const CaptureRequest& req)
{
    if (!resolveAnyConfigErrors()) {
        return nullptr;
    }

#if defined(Q_OS_MACOS)
    // This is required on MacOS because of Mission Control. If you'll switch to
    // another Desktop you cannot take a new screenshot from the tray, you have
    // to switch back to the Flameshot Desktop manually. It is not obvious and a
    // large number of users are confused and report a bug.
    if (m_captureWindow != nullptr) {
        m_captureWindow->close();
        delete m_captureWindow;
        m_captureWindow = nullptr;
    }
#endif

    if (nullptr == m_captureWindow) {
        // TODO is this unnecessary now?
        int timeout = 5000; // 5 seconds
        const int delay = 100;
        QWidget* modalWidget = nullptr;
        for (; timeout >= 0; timeout -= delay) {
            modalWidget = qApp->activeModalWidget();
            if (nullptr == modalWidget) {
                break;
            }
            modalWidget->close();
            modalWidget->deleteLater();
            QThread::msleep(delay);
        }
        if (0 == timeout) {
            QMessageBox::warning(
              nullptr, tr("Error"), tr("Unable to close active modal widgets"));
            return nullptr;
        }

        m_captureWindow = new CaptureWidget(req);

#ifdef Q_OS_WIN
        m_captureWindow->show();
#elif defined(Q_OS_MACOS)
        if (ConfigHandler().useNativeFullscreen()) {
            m_captureWindow->showFullScreen();
        } else {
            m_captureWindow->show();
        }
        m_captureWindow->activateWindow();
        m_captureWindow->raise();
#else
        m_captureWindow->showFullScreen();
//        m_captureWindow->show(); // For CaptureWidget Debugging under Linux
#endif
        return m_captureWindow;
    } else {
        emit captureFailed();
        return nullptr;
    }
}

void Flameshot::screen(CaptureRequest req, const int screenNumber)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    bool ok = false;
    QPixmap p;
    QRect geometry;

    if (screenNumber < 0) {
        ScreenGrabber grabber;
        p = grabber.grabEntireDesktop(ok);
        if (ok) {
            QScreen* selectedScreen = grabber.getSelectedScreen();
            if (selectedScreen) {
                geometry = ScreenGrabber().screenGeometry(selectedScreen);
            } else {
                ok = false;
            }
        }
    } else if (screenNumber >= qApp->screens().count()) {
        AbstractLogger() << QObject::tr(
          "Requested screen exceeds screen count");
        ok = false;
    } else {
        // Specific screen number provided - use grabScreen to bypass selector
        QScreen* screen = qApp->screens()[screenNumber];
        p = ScreenGrabber().grabScreen(screen, ok);
        if (ok) {
            geometry = ScreenGrabber().screenGeometry(screen);
        }
    }

    if (ok) {
        QRect region = req.initialSelection();
        if (region.isNull()) {
            region = geometry;
        } else {
            QRect screenGeom = geometry;
            screenGeom.moveTopLeft({ 0, 0 });
            region = region.intersected(screenGeom);
            p = p.copy(region);
        }
        if (req.tasks() & CaptureRequest::PIN) {
            // change geometry for pin task
            req.addPinTask(region);
        }
        exportCapture(p, geometry, req);
    } else {
        emit captureFailed();
    }
}

void Flameshot::full(const CaptureRequest& req)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    bool ok = true;
    QPixmap p(ScreenGrabber().grabFullDesktop(ok));
    if (ok) {
        QRect selection; // `flameshot full` does not support region selection
        exportCapture(p, selection, req);
    } else {
        emit captureFailed();
    }
}

void Flameshot::launcher()
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    if (m_launcherWindow == nullptr) {
        m_launcherWindow = new CaptureLauncher();
    }
    m_launcherWindow->show();
#if defined(Q_OS_MACOS)
    showDockIcon(m_launcherWindow);
#endif
}

void Flameshot::config()
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    if (m_configWindow == nullptr) {
        m_configWindow = new ConfigWindow();
        m_configWindow->show();
        // Call show() first, otherwise the correct geometry cannot be fetched
        // for centering the window on the screen
        QRect position = m_configWindow->frameGeometry();
        QScreen* currentScreen = QGuiAppCurrentScreen().currentScreen();
        position.moveCenter(currentScreen->availableGeometry().center());
        m_configWindow->move(position.topLeft());
#if defined(Q_OS_MACOS)
        showDockIcon(m_configWindow);
#endif
    }
}

void Flameshot::info()
{
    if (m_infoWindow == nullptr) {
        m_infoWindow = new InfoWindow();
#if defined(Q_OS_MACOS)
        showDockIcon(m_infoWindow);
#endif
    }
}

#ifdef ENABLE_IMGUR
void Flameshot::history()
{
    static UploadHistory* historyWidget = nullptr;
    if (historyWidget == nullptr) {
        historyWidget = new UploadHistory;
        historyWidget->loadHistory();
        connect(historyWidget, &QObject::destroyed, this, []() {
            historyWidget = nullptr;
        });
    }

    historyWidget->show();
    // Call show() first, otherwise the correct geometry cannot be fetched
    // for centering the window on the screen
    QRect position = historyWidget->frameGeometry();
    QScreen* currentScreen = QGuiAppCurrentScreen().currentScreen();
    position.moveCenter(currentScreen->availableGeometry().center());
    historyWidget->move(position.topLeft());

#if defined(Q_OS_MACOS)
    showDockIcon(historyWidget);
#endif
}
#endif

#if defined(Q_OS_MACOS)
void Flameshot::onWindowVisibilityChanged(QWindow::Visibility newVisibility)
{
    auto* qw = qobject_cast<QWindow*>(sender());
    if (!qw) {
        return;
    }

    if (newVisibility == QWindow::Hidden) {
        qw->setProperty(visibleInDockProperty, false);
        --m_dockIconVisibleCount;
        if (m_dockIconVisibleCount == 0) {
            setActivationPolicyAccessory();
        }
    } else {
        bool windowTrackedInDock = qw->property(visibleInDockProperty).toBool();
        if (!windowTrackedInDock) {
            qw->setProperty(visibleInDockProperty, true);
            ++m_dockIconVisibleCount;
            setActivationPolicyRegular();
        }
    }
}

void Flameshot::showDockIcon(QWidget* w)
{
    QWindow* qw = w->windowHandle();
    if (!qw) {
        return;
    }

    connect(qw,
            &QWindow::visibilityChanged,
            this,
            &Flameshot::onWindowVisibilityChanged);
}
#endif

void Flameshot::openSavePath()
{
    QString savePath = ConfigHandler().savePath();
    if (!savePath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
    }
}

QVersionNumber Flameshot::getVersion()
{
    return QVersionNumber::fromString(
      QStringLiteral(APP_VERSION).replace("v", ""));
}

void Flameshot::setOrigin(Origin origin)
{
    m_origin = origin;
}

Flameshot::Origin Flameshot::origin()
{
    return m_origin;
}

/**
 * @brief Prompt the user to resolve config errors if necessary.
 * @return Whether errors were resolved.
 */
bool Flameshot::resolveAnyConfigErrors()
{
    bool resolved = true;
    ConfigHandler confighandler;
    if (!confighandler.checkUnrecognizedSettings() ||
        !confighandler.checkSemantics()) {
        auto* resolver = new ConfigResolver();
        QObject::connect(
          resolver, &ConfigResolver::rejected, [resolver, &resolved]() {
              resolved = false;
              resolver->deleteLater();
              if (origin() == CLI) {
                  exit(1);
              }
          });
        QObject::connect(
          resolver, &ConfigResolver::accepted, [resolver, &resolved]() {
              resolved = true;
              resolver->close();
              resolver->deleteLater();
              // Ensure that the dialog is closed before starting capture
              qApp->processEvents();
          });
        resolver->exec();
        qApp->processEvents();
    }
    return resolved;
}

void Flameshot::requestCapture(const CaptureRequest& request)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    switch (request.captureMode()) {
        case CaptureRequest::FULLSCREEN_MODE:
            QTimer::singleShot(request.delay(),
                               [this, request] { full(request); });
            break;
        case CaptureRequest::SCREEN_MODE: {
            int&& number = request.data().toInt();
            QTimer::singleShot(request.delay(), [this, request, number]() {
                screen(request, number);
            });
            break;
        }
        case CaptureRequest::GRAPHICAL_MODE: {
            QTimer::singleShot(
              request.delay(), this, [this, request]() { gui(request); });
            break;
        }
        default:
            emit captureFailed();
            break;
    }
}

void Flameshot::exportCapture(const QPixmap& capture,
                              QRect& selection,
                              const CaptureRequest& req)
{
    using CR = CaptureRequest;
    int tasks = req.tasks(), mode = req.captureMode();
    QString path = req.path();

    if (tasks & CR::PRINT_GEOMETRY) {
        QTextStream(stdout)
          << selection.width() << "x" << selection.height() << "+"
          << selection.x() << "+" << selection.y() << "\n";
    }

    if (tasks & CR::PRINT_RAW) {
        QByteArray byteArray;
        QBuffer buffer(&byteArray);
        capture.save(&buffer, "PNG");
        if (QFile file; file.open(stdout, QIODevice::WriteOnly)) {
            file.write(byteArray);
            file.close();
        }
    }

    if (tasks & CR::SAVE) {
        if (req.path().isEmpty()) {
            saveToFilesystemGUI(capture);
        } else {
            saveToFilesystem(capture, path);
        }
    }

    if (tasks & CR::COPY) {
        FlameshotDaemon::copyToClipboard(capture);
    }

    if (tasks & CR::OCR) {
#ifdef ENABLE_OCR
        // 1. High-quality smooth scaling (2.5x - 3.0x) to optimize character resolution for Tesseract LSTM
        QImage srcImg = capture.toImage();
        const int origWidth = srcImg.width();
        const int origHeight = srcImg.height();

        qreal scaleFactor = 2.5;
        if (origWidth < 350 || origHeight < 350) {
            scaleFactor = 3.0;
        } else if (origWidth > 1600 || origHeight > 1600) {
            scaleFactor = 1.8;
        }

        const int targetW = qMax(1, qRound(origWidth * scaleFactor));
        const int targetH = qMax(1, qRound(origHeight * scaleFactor));
        QImage img = srcImg.scaled(targetW, targetH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        img = img.convertToFormat(QImage::Format_Grayscale8);

        const int width = img.width();
        const int height = img.height();

        // 2. Measure luminance distribution and detect background polarity
        long long globalSum = 0;
        int minVal = 255;
        int maxVal = 0;
        for (int y = 0; y < height; ++y) {
            const uchar* line = img.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                const uchar val = line[x];
                globalSum += val;
                if (val < minVal) minVal = val;
                if (val > maxVal) maxVal = val;
            }
        }
        const bool isLightBg = (globalSum / (static_cast<long long>(width) * height)) > 127;

        // 3. Normalize grayscale contrast and invert dark mode while preserving anti-aliased subpixel edges
        QImage procImg(width, height, QImage::Format_Grayscale8);
        const int range = qMax(1, maxVal - minVal);

        for (int y = 0; y < height; ++y) {
            const uchar* srcLine = img.constScanLine(y);
            uchar* dstLine = procImg.scanLine(y);
            for (int x = 0; x < width; ++x) {
                const int v = srcLine[x];
                int norm = (v - minVal) * 255 / range;
                norm = qBound(0, norm, 255);
                if (!isLightBg) {
                    // Dark mode: invert so text is dark on pure light background
                    dstLine[x] = static_cast<uchar>(255 - norm);
                } else {
                    dstLine[x] = static_cast<uchar>(norm);
                }
            }
        }

        // 4. Detect installed multi-language models (e.g. English, Bengali, Hindi, Tamil, Asian, European)
        QStringList targetLangs = { "eng", "osd", "ben", "hin", "tam", "tel", "kan", "mal", "mar", "guj", "pan", "ara", "rus", "deu", "fra", "spa", "ita", "chi_sim", "jpn", "kor" };
        QStringList activeLangs;
        const QStringList searchDirs = {
            QStringLiteral("/usr/share/tesseract-ocr/5/tessdata/"),
            QStringLiteral("/usr/share/tesseract-ocr/4.00/tessdata/"),
            QStringLiteral("/usr/share/tessdata/")
        };
        for (const QString& l : targetLangs) {
            for (const QString& dir : searchDirs) {
                if (QFile::exists(dir + l + QStringLiteral(".traineddata"))) {
                    if (!activeLangs.contains(l)) {
                        activeLangs.append(l);
                    }
                    break;
                }
            }
        }
        if (activeLangs.isEmpty()) {
            activeLangs.append(QStringLiteral("eng"));
        }
        QByteArray langBytes = activeLangs.join(QStringLiteral("+")).toUtf8();

        auto* api = new tesseract::TessBaseAPI();
        int initStatus = api->Init(nullptr, langBytes.constData());
        if (initStatus != 0) {
            // Fallback to English if combined multi-language loading fails
            initStatus = api->Init(nullptr, "eng");
        }

        if (initStatus == 0) {
            api->SetVariable("load_system_dawg", "0");
            api->SetVariable("load_freq_dawg", "0");
            api->SetVariable("user_defined_dpi", "300");
            api->SetVariable("preserve_interword_spaces", "1");
            api->SetPageSegMode(tesseract::PSM_AUTO);

            api->SetImage(procImg.bits(), procImg.width(), procImg.height(), 1, procImg.bytesPerLine());
            api->Recognize(0);

            char* outText = api->GetUTF8Text();
            QString result = QString::fromUtf8(outText).trimmed();
            delete[] outText;

            // Fallback: If full page detection found nothing, try sparse text / single block mode
            if (result.isEmpty()) {
                api->SetPageSegMode(tesseract::PSM_SPARSE_TEXT_OSD);
                api->SetImage(procImg.bits(), procImg.width(), procImg.height(), 1, procImg.bytesPerLine());
                api->Recognize(0);
                outText = api->GetUTF8Text();
                result = QString::fromUtf8(outText).trimmed();
                delete[] outText;
            }

            // Smart regex post-processing:
            // 1. Normalize line-leading bullet points
            result.replace(QRegularExpression(QStringLiteral("(^|\\n)[e◦o°*+•»›-]\\s+(?=[A-Za-z0-9\\p{L}])")), QStringLiteral("\\1• "));
            // 2. Clean up common misrecognized emoji artifacts before words at start of line or bullet (e.g. '@ Copy', 'W@ Copy', '€ Translate', 'xs Pin', '© Copy', '® Save')
            result.replace(QRegularExpression(QStringLiteral("(^|\\n|(?:•\\s+))(?:W@|@|€|xs|©|®|¢|¥|§|¶|~|\\|)\\s*(?=[A-Z\\p{Lu}])")), QStringLiteral("\\1"));
            // 3. Remove leading noise prefix symbols on isolated words like '@Copy' -> 'Copy'
            result.replace(QRegularExpression(QStringLiteral("(^|\\s)[@©®¢¥~|](?=[A-Za-z\\p{L}]{2,})")), QStringLiteral("\\1"));
            // 4. Clean up trailing OCR noise characters
            result.replace(QRegularExpression(QStringLiteral("[|~_]+(\\n|$)")), QStringLiteral("\\1"));
            result = result.trimmed();

            // 5. Extract structured word and line bounding boxes for interactive UI overlay
            QVector<QRect> lineBoxes;
            QStringList lines;
            QVector<QRect> wordBoxes;
            QStringList words;

            tesseract::ResultIterator* ri = api->GetIterator();
            if (ri) {
                // Line extraction
                do {
                    const char* lText = ri->GetUTF8Text(tesseract::RIL_TEXTLINE);
                    if (lText) {
                        QString lStr = QString::fromUtf8(lText).trimmed();
                        delete[] lText;
                        // Clean emoji artifacts on line level
                        lStr.replace(QRegularExpression(QStringLiteral("(^|\\n)[e◦o°*+•»›-]\\s+(?=[A-Za-z0-9\\p{L}])")), QStringLiteral("\\1• "));
                        lStr.replace(QRegularExpression(QStringLiteral("(^|\\n|(?:•\\s+))(?:W@|@|€|xs|©|®|¢|¥|§|¶|~|\\|)\\s*(?=[A-Z\\p{Lu}])")), QStringLiteral("\\1"));
                        lStr.replace(QRegularExpression(QStringLiteral("(^|\\s)[@©®¢¥~|](?=[A-Za-z\\p{L}]{2,})")), QStringLiteral("\\1"));
                        lStr = lStr.trimmed();
                        if (!lStr.isEmpty()) {
                            int x1, y1, x2, y2;
                            ri->BoundingBox(tesseract::RIL_TEXTLINE, &x1, &y1, &x2, &y2);
                            int bx = qRound(x1 / scaleFactor);
                            int by = qRound(y1 / scaleFactor);
                            int bw = qMax(1, qRound((x2 - x1) / scaleFactor));
                            int bh = qMax(1, qRound((y2 - y1) / scaleFactor));
                            lineBoxes.append(QRect(bx, by, bw, bh));
                            lines.append(lStr);
                        }
                    }
                } while (ri->Next(tesseract::RIL_TEXTLINE));

                // Word extraction
                ri = api->GetIterator();
                if (ri) {
                    do {
                        const char* wText = ri->GetUTF8Text(tesseract::RIL_WORD);
                        if (wText) {
                            QString wStr = QString::fromUtf8(wText).trimmed();
                            delete[] wText;
                            wStr.replace(QRegularExpression(QStringLiteral("^[@©®¢¥~|](?=[A-Za-z\\p{L}])")), QString());
                            wStr = wStr.trimmed();
                            if (!wStr.isEmpty()) {
                                int x1, y1, x2, y2;
                                ri->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
                                int bx = qRound(x1 / scaleFactor);
                                int by = qRound(y1 / scaleFactor);
                                int bw = qMax(1, qRound((x2 - x1) / scaleFactor));
                                int bh = qMax(1, qRound((y2 - y1) / scaleFactor));
                                wordBoxes.append(QRect(bx, by, bw, bh));
                                words.append(wStr);
                            }
                        }
                    } while (ri->Next(tesseract::RIL_WORD));
                }
            }

            api->End();
            delete api;

            if (!result.isEmpty()) {
                FlameshotDaemon::copyToClipboard(result, tr("Text extracted and copied to clipboard."));
                FlameshotDaemon::createOcr(capture, selection, result, wordBoxes, words, lineBoxes, lines);
            } else {
                AbstractLogger::warning(AbstractLogger::Target::Notification)
                  << tr("OCR found no text in the selection.");
            }
        } else {
            delete api;
            AbstractLogger::error(AbstractLogger::Target::Notification)
              << tr("Failed to initialize Tesseract OCR engine.");
        }
#else
        AbstractLogger::error(AbstractLogger::Target::Notification)
          << tr("OCR feature is disabled in this build of Flameshot.");
#endif
    }

    if (tasks & CR::PIN) {
        FlameshotDaemon::createPin(capture, selection);
        if (mode == CR::SCREEN_MODE || mode == CR::FULLSCREEN_MODE) {
            AbstractLogger::info()
              << QObject::tr("Full screen screenshot pinned to screen");
        }
    }

#ifdef ENABLE_IMGUR
    if (tasks & CR::UPLOAD) {
        if (!ConfigHandler().uploadWithoutConfirmation()) {
            auto* dialog = new ImgUploadDialog();
            if (dialog->exec() == QDialog::Rejected) {
                return;
            }
        }

        ImgUploaderBase* widget = ImgUploaderManager().uploader(capture);
        widget->show();
        widget->activateWindow();
        // NOTE: lambda can't capture 'this' because it might be destroyed later
        CR::ExportTask tasks = tasks;
        QObject::connect(
          widget, &ImgUploaderBase::uploadOk, [=, this](const QUrl& url) {
              if (ConfigHandler().copyURLAfterUpload()) {
                  if (!(tasks & CR::COPY)) {
                      FlameshotDaemon::copyToClipboard(
                        url.toString(), tr("URL copied to clipboard."));
                  }
                  widget->showPostUploadDialog();
              }
          });
    }
#endif

    if (!(tasks & CR::UPLOAD)) {
        emit captureTaken(capture);
    }
}

void Flameshot::setExternalWidget(bool b)
{
    m_haveExternalWidget = b;
}
bool Flameshot::haveExternalWidget()
{
    return m_haveExternalWidget;
}

// STATIC ATTRIBUTES
Flameshot::Origin Flameshot::m_origin = Flameshot::DAEMON;
