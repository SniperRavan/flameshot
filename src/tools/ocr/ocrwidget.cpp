// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "ocrwidget.h"
#include "core/flameshotdaemon.h"
#include "core/qguiappcurrentscreen.h"
#include "utils/confighandler.h"
#include "utils/globalvalues.h"
#include "utils/screenshotsaver.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QSplitter>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#ifdef ENABLE_OCR

// ============================================================================
// OcrCanvas Implementation
// ============================================================================

OcrCanvas::OcrCanvas(const QPixmap& pixmap,
                     const QVector<QRect>& wordBoxes,
                     const QStringList& words,
                     const QVector<QRect>& lineBoxes,
                     const QStringList& lines,
                     QWidget* parent)
  : QWidget(parent)
  , m_pixmap(pixmap)
  , m_wordBoxes(wordBoxes)
  , m_words(words)
  , m_lineBoxes(lineBoxes)
  , m_lines(lines)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
}

void OcrCanvas::setFilterQuery(const QString& query)
{
    m_filterQuery = query.trimmed();
    update();
}

void OcrCanvas::setHighlightBoxes(bool enabled)
{
    m_showBoxes = enabled;
    update();
}

void OcrCanvas::selectAllWords()
{
    m_selectedIndices.clear();
    for (int i = 0; i < m_words.size(); ++i) {
        m_selectedIndices.insert(i);
    }
    emit selectionChanged();
    update();
}

void OcrCanvas::clearSelection()
{
    m_selectedIndices.clear();
    emit selectionChanged();
    update();
}

QString OcrCanvas::selectedText() const
{
    if (m_selectedIndices.isEmpty()) {
        return QString();
    }
    QStringList sel;
    for (int i = 0; i < m_words.size(); ++i) {
        if (m_selectedIndices.contains(i)) {
            sel.append(m_words.at(i));
        }
    }
    return sel.join(QStringLiteral(" "));
}

QRect OcrCanvas::mapToCanvas(const QRect& r) const
{
    if (m_pixmap.isNull() || width() <= 0 || height() <= 0) {
        return QRect();
    }
    const qreal scaleX = static_cast<qreal>(width()) / m_pixmap.width();
    const qreal scaleY = static_cast<qreal>(height()) / m_pixmap.height();
    const qreal scale = qMin(scaleX, scaleY);

    const qreal offX = (width() - m_pixmap.width() * scale) / 2.0;
    const qreal offY = (height() - m_pixmap.height() * scale) / 2.0;

    return QRect(qRound(offX + r.x() * scale),
                 qRound(offY + r.y() * scale),
                 qMax(2, qRound(r.width() * scale)),
                 qMax(2, qRound(r.height() * scale)));
}

QRect OcrCanvas::mapFromCanvas(const QRect& r) const
{
    if (m_pixmap.isNull() || width() <= 0 || height() <= 0) {
        return QRect();
    }
    const qreal scaleX = static_cast<qreal>(width()) / m_pixmap.width();
    const qreal scaleY = static_cast<qreal>(height()) / m_pixmap.height();
    const qreal scale = qMin(scaleX, scaleY);

    if (scale <= 0.0) {
        return QRect();
    }

    const qreal offX = (width() - m_pixmap.width() * scale) / 2.0;
    const qreal offY = (height() - m_pixmap.height() * scale) / 2.0;

    return QRect(qRound((r.x() - offX) / scale),
                 qRound((r.y() - offY) / scale),
                 qRound(r.width() / scale),
                 qRound(r.height() / scale));
}

int OcrCanvas::findWordAt(const QPoint& pos) const
{
    for (int i = 0; i < m_wordBoxes.size(); ++i) {
        QRect canvasRect = mapToCanvas(m_wordBoxes.at(i));
        // Add 2px margin for easy selection
        if (canvasRect.adjusted(-2, -2, 2, 2).contains(pos)) {
            return i;
        }
    }
    return -1;
}

void OcrCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Canvas background
    p.fillRect(rect(), QColor(22, 24, 29));

    if (m_pixmap.isNull()) {
        return;
    }

    const qreal scaleX = static_cast<qreal>(width()) / m_pixmap.width();
    const qreal scaleY = static_cast<qreal>(height()) / m_pixmap.height();
    const qreal scale = qMin(scaleX, scaleY);

    const int targetW = qRound(m_pixmap.width() * scale);
    const int targetH = qRound(m_pixmap.height() * scale);
    const int offX = (width() - targetW) / 2;
    const int offY = (height() - targetH) / 2;
    const QRect imgRect(offX, offY, targetW, targetH);

    // Draw capture image
    p.drawPixmap(imgRect, m_pixmap);

    // Subtle dark overlay to let bounding boxes pop
    p.fillRect(imgRect, QColor(0, 0, 0, 30));

    ConfigHandler conf;
    const QColor baseColor = conf.uiColor();

    if (m_showBoxes) {
        // Draw word boxes
        for (int i = 0; i < m_wordBoxes.size(); ++i) {
            QRect cRect = mapToCanvas(m_wordBoxes.at(i));
            if (!cRect.isValid()) continue;

            bool isSelected = m_selectedIndices.contains(i);
            bool isHovered = (i == m_hoveredIndex);
            bool isSearchMatch = (!m_filterQuery.isEmpty() &&
                                  m_words.value(i).contains(m_filterQuery, Qt::CaseInsensitive));

            QPainterPath path;
            path.addRoundedRect(cRect, 3, 3);

            if (isSearchMatch) {
                // Gold / Amber glow for search matches
                p.fillPath(path, QColor(255, 193, 7, 140));
                p.strokePath(path, QPen(QColor(255, 235, 59), 2));
            } else if (isSelected) {
                // Vivid accent for selected words
                p.fillPath(path, QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 130));
                p.strokePath(path, QPen(baseColor.lighter(130), 1.8));
            } else if (isHovered) {
                // Bright hover box
                p.fillPath(path, QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 80));
                p.strokePath(path, QPen(baseColor.lighter(150), 1.5));
            } else {
                // Subtle ambient box (Google Lens / Windows OCR style)
                p.fillPath(path, QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 25));
                p.strokePath(path, QPen(QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 90), 1));
            }
        }
    }

    // Drag selection marquee box
    if (m_isSelecting) {
        QRect selRect = QRect(m_selectionStart, m_selectionCurrent).normalized();
        p.fillRect(selRect, QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 50));
        p.setPen(QPen(baseColor, 1.5, Qt::DashLine));
        p.drawRect(selRect);
    }
}

void OcrCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_selectionStart = event->pos();
        m_selectionCurrent = event->pos();
        m_isSelecting = true;

        int idx = findWordAt(event->pos());
        if (idx >= 0) {
            if (event->modifiers() & Qt::ControlModifier) {
                if (m_selectedIndices.contains(idx)) {
                    m_selectedIndices.remove(idx);
                } else {
                    m_selectedIndices.insert(idx);
                }
            } else {
                m_selectedIndices.clear();
                m_selectedIndices.insert(idx);
            }
            emit selectionChanged();
            update();
        } else if (!(event->modifiers() & Qt::ControlModifier)) {
            m_selectedIndices.clear();
            emit selectionChanged();
            update();
        }
    }
}

void OcrCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (m_isSelecting) {
        m_selectionCurrent = event->pos();
        QRect selRect = QRect(m_selectionStart, m_selectionCurrent).normalized();

        for (int i = 0; i < m_wordBoxes.size(); ++i) {
            QRect cRect = mapToCanvas(m_wordBoxes.at(i));
            if (selRect.intersects(cRect)) {
                m_selectedIndices.insert(i);
            }
        }
        emit selectionChanged();
        update();
        return;
    }

    int idx = findWordAt(event->pos());
    if (idx != m_hoveredIndex) {
        m_hoveredIndex = idx;
        if (idx >= 0 && idx < m_words.size()) {
            QToolTip::showText(event->globalPosition().toPoint(),
                               m_words.at(idx),
                               this,
                               mapToCanvas(m_wordBoxes.at(idx)));
        } else {
            QToolTip::hideText();
        }
        update();
    }
}

void OcrCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_isSelecting) {
        m_isSelecting = false;
        update();
    }
}

void OcrCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        int idx = findWordAt(event->pos());
        if (idx >= 0 && idx < m_words.size()) {
            QString word = m_words.at(idx);
            QApplication::clipboard()->setText(word);
            emit wordDoubleClicked(word);
            emit statusMessageRequested(tr("Word copied: \"%1\"").arg(word));
        }
    }
}

void OcrCanvas::leaveEvent(QEvent*)
{
    if (m_hoveredIndex != -1) {
        m_hoveredIndex = -1;
        QToolTip::hideText();
        update();
    }
}

void OcrCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    menu.setStyleSheet(
      QStringLiteral("QMenu { background-color: #23272e; color: #abb2bf; border: 1px solid #3b4048; border-radius: 6px; padding: 4px; }"
                     "QMenu::item { padding: 6px 20px; border-radius: 4px; }"
                     "QMenu::item:selected { background-color: #3e4451; color: #ffffff; }"));

    int clickedIdx = findWordAt(event->pos());
    if (clickedIdx >= 0 && !m_selectedIndices.contains(clickedIdx)) {
        m_selectedIndices.clear();
        m_selectedIndices.insert(clickedIdx);
        emit selectionChanged();
        update();
    }

    QString selText = selectedText();
    if (!selText.isEmpty()) {
        QAction* copySelAct = menu.addAction(tr("Copy Selected (%1 words)").arg(m_selectedIndices.size()));
        connect(copySelAct, &QAction::triggered, this, [this, selText]() {
            QApplication::clipboard()->setText(selText);
            emit statusMessageRequested(tr("Copied %1 words to clipboard").arg(m_selectedIndices.size()));
        });

        QAction* searchSelAct = menu.addAction(tr("Search Selected on Google"));
        connect(searchSelAct, &QAction::triggered, this, [selText]() {
            QUrl url(QStringLiteral("https://www.google.com/search?q=") + QUrl::toPercentEncoding(selText));
            QDesktopServices::openUrl(url);
        });

        QAction* transSelAct = menu.addAction(tr("Translate Selected"));
        connect(transSelAct, &QAction::triggered, this, [selText]() {
            QUrl url(QStringLiteral("https://translate.google.com/?sl=auto&tl=en&text=") + QUrl::toPercentEncoding(selText));
            QDesktopServices::openUrl(url);
        });

        menu.addSeparator();
    }

    QAction* selectAllAct = menu.addAction(tr("Select All Words"));
    connect(selectAllAct, &QAction::triggered, this, &OcrCanvas::selectAllWords);

    if (!m_selectedIndices.isEmpty()) {
        QAction* clearSelAct = menu.addAction(tr("Clear Selection"));
        connect(clearSelAct, &QAction::triggered, this, &OcrCanvas::clearSelection);
    }

    menu.exec(event->globalPos());
}

// ============================================================================
// OcrWidget Implementation
// ============================================================================

OcrWidget::OcrWidget(const QPixmap& pixmap,
                     const QRect& geometry,
                     const QString& fullText,
                     const QVector<QRect>& wordBoxes,
                     const QStringList& words,
                     const QVector<QRect>& lineBoxes,
                     const QStringList& lines,
                     QWidget* parent)
  : QWidget(parent)
  , m_pixmap(pixmap)
  , m_geometry(geometry)
  , m_fullText(fullText)
  , m_wordBoxes(wordBoxes)
  , m_words(words)
  , m_lineBoxes(lineBoxes)
  , m_lines(lines)
  , m_toastTimer(new QTimer(this))
{
    setWindowIcon(QIcon(GlobalValues::iconPath()));
    setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Flameshot Text Recognition (OCR)"));
    setFocusPolicy(Qt::StrongFocus);

    ConfigHandler conf;
    m_accentColor = conf.uiColor();
    m_contrastColor = conf.contrastUiColor();

    setupUi();
    applyTheme();

    // Keyboard Shortcuts
    new QShortcut(QKeySequence(Qt::Key_Escape), this, SLOT(close()));
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q), this, SLOT(close()));
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this, SLOT(searchGoogle()));
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this, SLOT(translateGoogle()));
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_S), this, SLOT(saveToFile()));

    // Center on screen or near capture geometry
    QScreen* screen = QGuiAppCurrentScreen().currentScreen();
    QRect screenGeom = screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
    int targetW = qBound(680, qRound(screenGeom.width() * 0.55), 1100);
    int targetH = qBound(460, qRound(screenGeom.height() * 0.55), 750);
    resize(targetW, targetH);

    int posX = screenGeom.x() + (screenGeom.width() - targetW) / 2;
    int posY = screenGeom.y() + (screenGeom.height() - targetH) / 2;
    move(posX, posY);

    // Initial toast
    showToast(tr("Text extracted and copied to clipboard!"));
}

void OcrWidget::setupUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 10, 10, 10);

    auto* mainContainer = new QWidget(this);
    mainContainer->setObjectName(QStringLiteral("mainContainer"));
    auto* containerLayout = new QVBoxLayout(mainContainer);
    containerLayout->setContentsMargins(14, 12, 14, 12);
    containerLayout->setSpacing(8);

    // ------------------------------------------------------------------------
    // 1. Header Bar
    // ------------------------------------------------------------------------
    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(10);

    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(QIcon(GlobalValues::iconPath()).pixmap(20, 20));
    headerLayout->addWidget(iconLabel);

    auto* titleLabel = new QLabel(tr("Text Recognition (OCR)"), this);
    titleLabel->setObjectName(QStringLiteral("titleLabel"));
    headerLayout->addWidget(titleLabel);

    m_statsBadge = new QLabel(tr("%1 words • %2 lines").arg(m_words.size()).arg(m_lines.size()), this);
    m_statsBadge->setObjectName(QStringLiteral("statsBadge"));
    headerLayout->addWidget(m_statsBadge);

    headerLayout->addStretch();

    auto* closeBtn = new QPushButton(QStringLiteral("✕"), this);
    closeBtn->setObjectName(QStringLiteral("closeBtn"));
    closeBtn->setFixedSize(28, 28);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setToolTip(tr("Close (Esc)"));
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    headerLayout->addWidget(closeBtn);

    containerLayout->addLayout(headerLayout);

    // ------------------------------------------------------------------------
    // 2. Toast Notification Banner
    // ------------------------------------------------------------------------
    m_toastLabel = new QLabel(this);
    m_toastLabel->setObjectName(QStringLiteral("toastLabel"));
    m_toastLabel->setAlignment(Qt::AlignCenter);
    m_toastLabel->setVisible(false);
    containerLayout->addWidget(m_toastLabel);

    connect(m_toastTimer, &QTimer::timeout, this, [this]() {
        m_toastLabel->setVisible(false);
        m_toastTimer->stop();
    });

    // ------------------------------------------------------------------------
    // 3. Central Splitter (Visual Canvas + Text Panel)
    // ------------------------------------------------------------------------
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("splitter"));
    splitter->setChildrenCollapsible(false);

    // Left Pane: Visual Canvas
    m_canvas = new OcrCanvas(m_pixmap, m_wordBoxes, m_words, m_lineBoxes, m_lines, this);
    connect(m_canvas, &OcrCanvas::wordDoubleClicked, this, [this](const QString& w) {
        showToast(tr("Copied word: \"%1\"").arg(w));
    });
    connect(m_canvas, &OcrCanvas::statusMessageRequested, this, &OcrWidget::showToast);
    splitter->addWidget(m_canvas);

    // Right Pane: Text Editor & Inspector
    auto* rightPanel = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(4, 0, 0, 0);
    rightLayout->setSpacing(6);

    // Search bar
    m_searchBox = new QLineEdit(this);
    m_searchBox->setObjectName(QStringLiteral("searchBox"));
    m_searchBox->setPlaceholderText(tr("🔍 Find in text..."));
    m_searchBox->setClearButtonEnabled(true);
    connect(m_searchBox, &QLineEdit::textChanged, this, &OcrWidget::onSearchTextChanged);
    rightLayout->addWidget(m_searchBox);

    // Format mode selector tabs
    auto* formatLayout = new QHBoxLayout();
    formatLayout->setSpacing(4);

    auto* fmtBtn = new QPushButton(tr("Formatted"), this);
    auto* lineBtn = new QPushButton(tr("Line by Line"), this);
    auto* singleBtn = new QPushButton(tr("Single Line"), this);

    fmtBtn->setCheckable(true);
    lineBtn->setCheckable(true);
    singleBtn->setCheckable(true);
    fmtBtn->setChecked(true);

    fmtBtn->setObjectName(QStringLiteral("fmtBtn"));
    lineBtn->setObjectName(QStringLiteral("lineBtn"));
    singleBtn->setObjectName(QStringLiteral("singleBtn"));

    formatLayout->addWidget(fmtBtn);
    formatLayout->addWidget(lineBtn);
    formatLayout->addWidget(singleBtn);
    rightLayout->addLayout(formatLayout);

    connect(fmtBtn, &QPushButton::clicked, this, [=, this]() {
        fmtBtn->setChecked(true);
        lineBtn->setChecked(false);
        singleBtn->setChecked(false);
        setFormatMode(0);
    });
    connect(lineBtn, &QPushButton::clicked, this, [=, this]() {
        fmtBtn->setChecked(false);
        lineBtn->setChecked(true);
        singleBtn->setChecked(false);
        setFormatMode(1);
    });
    connect(singleBtn, &QPushButton::clicked, this, [=, this]() {
        fmtBtn->setChecked(false);
        lineBtn->setChecked(false);
        singleBtn->setChecked(true);
        setFormatMode(2);
    });

    // Text Editor
    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setObjectName(QStringLiteral("textEdit"));
    m_textEdit->setPlainText(m_fullText);
    rightLayout->addWidget(m_textEdit);

    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    containerLayout->addWidget(splitter, 1);

    // ------------------------------------------------------------------------
    // 4. Action Toolbar (Bottom Bar)
    // ------------------------------------------------------------------------
    auto* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setSpacing(8);

    auto* copyAllBtn = new QPushButton(tr("📋 Copy All"), this);
    copyAllBtn->setObjectName(QStringLiteral("copyAllBtn"));
    copyAllBtn->setCursor(Qt::PointingHandCursor);
    connect(copyAllBtn, &QPushButton::clicked, this, &OcrWidget::copyAllText);
    toolbarLayout->addWidget(copyAllBtn);

    auto* copySelBtn = new QPushButton(tr("🔠 Copy Selected"), this);
    copySelBtn->setObjectName(QStringLiteral("copySelBtn"));
    copySelBtn->setCursor(Qt::PointingHandCursor);
    connect(copySelBtn, &QPushButton::clicked, this, &OcrWidget::copySelectedText);
    toolbarLayout->addWidget(copySelBtn);

    auto* searchBtn = new QPushButton(tr("🔍 Google"), this);
    searchBtn->setObjectName(QStringLiteral("actionBtn"));
    searchBtn->setCursor(Qt::PointingHandCursor);
    searchBtn->setToolTip(tr("Search on Google (Ctrl+G)"));
    connect(searchBtn, &QPushButton::clicked, this, &OcrWidget::searchGoogle);
    toolbarLayout->addWidget(searchBtn);

    auto* transBtn = new QPushButton(tr("🌐 Translate"), this);
    transBtn->setObjectName(QStringLiteral("actionBtn"));
    transBtn->setCursor(Qt::PointingHandCursor);
    transBtn->setToolTip(tr("Translate with Google Translate (Ctrl+T)"));
    connect(transBtn, &QPushButton::clicked, this, &OcrWidget::translateGoogle);
    toolbarLayout->addWidget(transBtn);

    auto* saveBtn = new QPushButton(tr("💾 Save .txt"), this);
    saveBtn->setObjectName(QStringLiteral("actionBtn"));
    saveBtn->setCursor(Qt::PointingHandCursor);
    saveBtn->setToolTip(tr("Save text to a file (Ctrl+S)"));
    connect(saveBtn, &QPushButton::clicked, this, &OcrWidget::saveToFile);
    toolbarLayout->addWidget(saveBtn);

    auto* pinBtn = new QPushButton(tr("📌 Pin"), this);
    pinBtn->setObjectName(QStringLiteral("actionBtn"));
    pinBtn->setCursor(Qt::PointingHandCursor);
    pinBtn->setToolTip(tr("Pin image to desktop"));
    connect(pinBtn, &QPushButton::clicked, this, &OcrWidget::pinScreenshot);
    toolbarLayout->addWidget(pinBtn);

    toolbarLayout->addStretch();

    auto* doneBtn = new QPushButton(tr("Done"), this);
    doneBtn->setObjectName(QStringLiteral("doneBtn"));
    doneBtn->setCursor(Qt::PointingHandCursor);
    connect(doneBtn, &QPushButton::clicked, this, &QWidget::close);
    toolbarLayout->addWidget(doneBtn);

    containerLayout->addLayout(toolbarLayout);
    rootLayout->addWidget(mainContainer);

    // Drop shadow
    m_shadowEffect = new QGraphicsDropShadowEffect(this);
    m_shadowEffect->setColor(QColor(0, 0, 0, 160));
    m_shadowEffect->setBlurRadius(20);
    m_shadowEffect->setOffset(0, 6);
    mainContainer->setGraphicsEffect(m_shadowEffect);
}

void OcrWidget::applyTheme()
{
    QString accentHex = m_accentColor.name();
    QString accentRgba = QStringLiteral("rgba(%1, %2, %3, 0.9)")
                           .arg(m_accentColor.red())
                           .arg(m_accentColor.green())
                           .arg(m_accentColor.blue());

    setStyleSheet(QString(
      "#mainContainer {"
      "    background-color: #1e222b;"
      "    border: 1px solid #333842;"
      "    border-radius: 10px;"
      "}"
      "#titleLabel {"
      "    color: #ffffff;"
      "    font-size: 14px;"
      "    font-weight: bold;"
      "}"
      "#statsBadge {"
      "    color: #abb2bf;"
      "    background-color: #282c34;"
      "    border: 1px solid #3e4451;"
      "    border-radius: 10px;"
      "    padding: 2px 8px;"
      "    font-size: 11px;"
      "}"
      "#closeBtn {"
      "    background: transparent;"
      "    color: #abb2bf;"
      "    border: none;"
      "    border-radius: 14px;"
      "    font-size: 14px;"
      "}"
      "#closeBtn:hover {"
      "    background-color: #e06c75;"
      "    color: #ffffff;"
      "}"
      "#toastLabel {"
      "    background-color: %2;"
      "    color: #ffffff;"
      "    font-weight: bold;"
      "    border-radius: 6px;"
      "    padding: 6px 12px;"
      "    font-size: 12px;"
      "}"
      "#searchBox {"
      "    background-color: #282c34;"
      "    color: #abb2bf;"
      "    border: 1px solid #3e4451;"
      "    border-radius: 6px;"
      "    padding: 6px 10px;"
      "    font-size: 12px;"
      "}"
      "#searchBox:focus {"
      "    border: 1px solid %1;"
      "    color: #ffffff;"
      "}"
      "#fmtBtn, #lineBtn, #singleBtn {"
      "    background-color: #282c34;"
      "    color: #abb2bf;"
      "    border: 1px solid #3e4451;"
      "    border-radius: 4px;"
      "    padding: 4px 8px;"
      "    font-size: 11px;"
      "}"
      "#fmtBtn:checked, #lineBtn:checked, #singleBtn:checked {"
      "    background-color: %1;"
      "    color: #ffffff;"
      "    border: 1px solid %1;"
      "    font-weight: bold;"
      "}"
      "#textEdit {"
      "    background-color: #21252b;"
      "    color: #d7dae0;"
      "    border: 1px solid #333842;"
      "    border-radius: 6px;"
      "    padding: 8px;"
      "    font-family: 'Consolas', 'DejaVu Sans Mono', 'Monospace', monospace;"
      "    font-size: 12px;"
      "    line-height: 1.4;"
      "}"
      "#copyAllBtn {"
      "    background-color: %1;"
      "    color: #ffffff;"
      "    border: none;"
      "    border-radius: 6px;"
      "    padding: 6px 14px;"
      "    font-weight: bold;"
      "}"
      "#copyAllBtn:hover {"
      "    background-color: %2;"
      "}"
      "#copySelBtn, #actionBtn, #doneBtn {"
      "    background-color: #282c34;"
      "    color: #d7dae0;"
      "    border: 1px solid #3e4451;"
      "    border-radius: 6px;"
      "    padding: 6px 12px;"
      "    font-size: 12px;"
      "}"
      "#copySelBtn:hover, #actionBtn:hover, #doneBtn:hover {"
      "    background-color: #3e4451;"
      "    color: #ffffff;"
      "}")
      .arg(accentHex, accentRgba));
}

void OcrWidget::showToast(const QString& message)
{
    m_toastLabel->setText(message);
    m_toastLabel->setVisible(true);
    m_toastTimer->start(2800);
}

void OcrWidget::copyAllText()
{
    QString txt = m_textEdit->toPlainText();
    if (!txt.isEmpty()) {
        QApplication::clipboard()->setText(txt);
        showToast(tr("All text copied to clipboard!"));
    }
}

void OcrWidget::copySelectedText()
{
    QString txt = m_textEdit->textCursor().selectedText();
    if (txt.isEmpty() && m_canvas) {
        txt = m_canvas->selectedText();
    }
    if (!txt.isEmpty()) {
        QApplication::clipboard()->setText(txt);
        showToast(tr("Selected text copied to clipboard!"));
    } else {
        copyAllText();
    }
}

void OcrWidget::searchGoogle()
{
    QString txt = m_textEdit->textCursor().selectedText();
    if (txt.isEmpty()) {
        txt = m_textEdit->toPlainText();
    }
    if (!txt.isEmpty()) {
        QUrl url(QStringLiteral("https://www.google.com/search?q=") + QUrl::toPercentEncoding(txt.trimmed()));
        QDesktopServices::openUrl(url);
    }
}

void OcrWidget::translateGoogle()
{
    QString txt = m_textEdit->textCursor().selectedText();
    if (txt.isEmpty()) {
        txt = m_textEdit->toPlainText();
    }
    if (!txt.isEmpty()) {
        QUrl url(QStringLiteral("https://translate.google.com/?sl=auto&tl=en&text=") + QUrl::toPercentEncoding(txt.trimmed()));
        QDesktopServices::openUrl(url);
    }
}

void OcrWidget::saveToFile()
{
    QString defaultName = QStringLiteral("ocr_") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss")) + QStringLiteral(".txt");
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save OCR Text"), defaultName, tr("Text Files (*.txt);;All Files (*)"));
    if (!fileName.isEmpty()) {
        QFile file(fileName);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << m_textEdit->toPlainText();
            file.close();
            showToast(tr("Saved to %1").arg(QFileInfo(fileName).fileName()));
        }
    }
}

void OcrWidget::pinScreenshot()
{
    FlameshotDaemon::createPin(m_pixmap, m_geometry);
    close();
}

void OcrWidget::onSearchTextChanged(const QString& text)
{
    if (m_canvas) {
        m_canvas->setFilterQuery(text);
    }
}

void OcrWidget::setFormatMode(int mode)
{
    m_currentFormatMode = mode;
    if (mode == 0) {
        m_textEdit->setPlainText(m_fullText);
    } else if (mode == 1) {
        m_textEdit->setPlainText(m_lines.join(QStringLiteral("\n")));
    } else if (mode == 2) {
        m_textEdit->setPlainText(m_words.join(QStringLiteral(" ")));
    }
}

void OcrWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_isDraggingWindow = true;
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
    }
}

void OcrWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_isDraggingWindow && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragPosition);
    }
}

void OcrWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_isDraggingWindow = false;
    }
}

void OcrWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || (event->modifiers() & Qt::ControlModifier && event->key() == Qt::Key_Q)) {
        close();
    } else {
        QWidget::keyPressEvent(event);
    }
}

#endif
