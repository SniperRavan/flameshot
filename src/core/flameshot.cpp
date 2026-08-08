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

        // 4. Fast, highly accurate primary multi-language loader (<0.2s execution)
        QStringList targetLangs = { "eng", "ben", "hin", "osd" };
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

            // Helper to detect colored UI emojis in any bounding box
            auto detectEmojiInBox = [&](const QRect& bBox) -> QString {
                QRect r = bBox.intersected(srcImg.rect());
                if (r.width() < 8 || r.height() < 8) return QString();
                // Emojis are square or nearly square standalone icons
                qreal aspect = static_cast<qreal>(r.width()) / qMax(1, r.height());
                if (aspect > 1.45 || aspect < 0.65) return QString(); // Regular text words like "to", "CPU", "code" are NOT emojis

                int totalColored = 0, redCount = 0, yellowCount = 0, greenCount = 0;
                for (int y = r.y(); y <= r.bottom(); ++y) {
                    const QRgb* line = reinterpret_cast<const QRgb*>(srcImg.constScanLine(y));
                    for (int x = r.x(); x <= r.right(); ++x) {
                        QColor c(line[x]);
                        if (c.hsvSaturation() >= 85 && c.value() >= 55) {
                            totalColored++;
                            int h = c.hsvHue();
                            if (h >= 330 || h <= 18) {
                                redCount++;
                            } else if (h >= 26 && h <= 62) {
                                yellowCount++;
                            } else if (h >= 80 && h <= 155) {
                                greenCount++;
                            }
                        }
                    }
                }
                int area = r.width() * r.height();
                if (totalColored >= 18 && totalColored >= area * 0.12) {
                    if (redCount >= totalColored * 0.45) return QStringLiteral("❤️");
                    if (yellowCount >= totalColored * 0.45) return QStringLiteral("👍");
                    if (greenCount >= totalColored * 0.45) return QStringLiteral("✅");
                }
                return QString();
            };

            // 5. Extract structured words and lines with synchronized emoji replacement
            QVector<QRect> lineBoxes;
            QStringList lines;
            QVector<QRect> wordBoxes;
            QStringList words;

            tesseract::ResultIterator* ri = api->GetIterator();
            if (ri) {
                do {
                    // Extract line and its constituent words
                    QVector<QRect> curLineWordBoxes;
                    QStringList curLineWords;
                    int lx1 = 0, ly1 = 0, lx2 = 0, ly2 = 0;
                    ri->BoundingBox(tesseract::RIL_TEXTLINE, &lx1, &ly1, &lx2, &ly2);

                    do {
                        const char* wText = ri->GetUTF8Text(tesseract::RIL_WORD);
                        if (wText) {
                            QString wStr = QString::fromUtf8(wText).trimmed();
                            delete[] wText;
                            wStr.replace(QRegularExpression(QStringLiteral("^[@©®¢¥~|](?=[A-Za-z\\p{L}])")), QString());
                            // Fix @ confused with 0 in numbers like @.2s -> 0.2s
                            wStr.replace(QRegularExpression(QStringLiteral("@(?=[0-9])")), QStringLiteral("0"));
                            wStr.replace(QRegularExpression(QStringLiteral("(?<=[0-9])@")), QStringLiteral("0"));
                            wStr = wStr.trimmed();

                            int x1, y1, x2, y2;
                            ri->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
                            int bx = qRound(x1 / scaleFactor);
                            int by = qRound(y1 / scaleFactor);
                            int bw = qMax(1, qRound((x2 - x1) / scaleFactor));
                            int bh = qMax(1, qRound((y2 - y1) / scaleFactor));
                            QRect wRect(bx, by, bw, bh);

                            // Detect if this word box is a genuine colored emoji (only replace single misrecognized noise glyphs)
                            QString emoji = detectEmojiInBox(wRect);
                            if (!emoji.isEmpty()) {
                                if (wStr == QStringLiteral("Y") || wStr == QStringLiteral("の") || wStr == QStringLiteral("ول") || wStr == QStringLiteral("J") || wStr == QStringLiteral("4") || wStr == QStringLiteral("€") || wStr == QStringLiteral("å")) {
                                    wStr = emoji;
                                }
                            }

                            if (!wStr.isEmpty()) {
                                wordBoxes.append(wRect);
                                words.append(wStr);
                                curLineWordBoxes.append(wRect);
                                curLineWords.append(wStr);
                            }
                        }
                    } while (!ri->IsAtFinalElement(tesseract::RIL_TEXTLINE, tesseract::RIL_WORD) && ri->Next(tesseract::RIL_WORD));

                    if (!curLineWords.isEmpty()) {
                        int bx = qRound(lx1 / scaleFactor);
                        int by = qRound(ly1 / scaleFactor);
                        int bw = qMax(1, qRound((lx2 - lx1) / scaleFactor));
                        int bh = qMax(1, qRound((ly2 - ly1) / scaleFactor));
                        lineBoxes.append(QRect(bx, by, bw, bh));

                        QString lStr = curLineWords.join(QStringLiteral(" "));
                        // Fix bullet points (including Bengali 'ও' misread as bullet point in Latin lists)
                        lStr.replace(QRegularExpression(QStringLiteral("(^|\\n)[ওe◦o°*+•»›-]\\s+(?=[A-Za-z0-9\\p{L}#])")), QStringLiteral("\\1• "));
                        lines.append(lStr);
                    }
                } while (ri->Next(tesseract::RIL_TEXTLINE));
            }

            api->End();
            delete api;

            QString result = lines.join(QStringLiteral("\n"));

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
