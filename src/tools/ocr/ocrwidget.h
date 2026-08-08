// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QWidget>
#include <QPixmap>
#include <QRect>
#include <QStringList>
#include <QVector>
#include <QSet>

class QLabel;
class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QSplitter;
class QVBoxLayout;
class QHBoxLayout;
class QGraphicsDropShadowEffect;
class QTimer;

#ifdef ENABLE_OCR

class OcrCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit OcrCanvas(const QPixmap& pixmap,
                       const QVector<QRect>& wordBoxes,
                       const QStringList& words,
                       const QVector<QRect>& lineBoxes,
                       const QStringList& lines,
                       QWidget* parent = nullptr);

    void setFilterQuery(const QString& query);
    void setHighlightBoxes(bool enabled);
    void selectAllWords();
    void clearSelection();
    QString selectedText() const;

    void zoomIn();
    void zoomOut();
    void zoomFit();
    void setZoomFactor(qreal factor);
    qreal zoomFactor() const { return m_zoomFactor; }

    QSize sizeHint() const override;

signals:
    void wordDoubleClicked(const QString& word);
    void selectionChanged();
    void statusMessageRequested(const QString& msg);
    void zoomChanged(qreal factor);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRect mapToCanvas(const QRect& r) const;
    QRect mapFromCanvas(const QRect& r) const;
    int findWordAt(const QPoint& pos) const;
    void updateCanvasSize();

    QPixmap m_pixmap;
    QVector<QRect> m_wordBoxes;
    QStringList m_words;
    QVector<QRect> m_lineBoxes;
    QStringList m_lines;

    int m_hoveredIndex{ -1 };
    QSet<int> m_selectedIndices;
    QString m_filterQuery;
    bool m_showBoxes{ true };

    bool m_isSelecting{ false };
    QPoint m_selectionStart;
    QPoint m_selectionCurrent;

    qreal m_zoomFactor{ 1.0 };
    QPoint m_panOffset{ 0, 0 };
    bool m_isPanning{ false };
    QPoint m_panStart;
};

class OcrWidget : public QWidget
{
    Q_OBJECT
public:
    explicit OcrWidget(const QPixmap& pixmap,
                       const QRect& geometry,
                       const QString& fullText,
                       const QVector<QRect>& wordBoxes,
                       const QStringList& words,
                       const QVector<QRect>& lineBoxes,
                       const QStringList& lines,
                       QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void copyAllText();
    void copySelectedText();
    void searchWeb();
    void translateText();
    void saveToFile();
    void pinScreenshot();
    void showToast(const QString& message);
    void onSearchTextChanged(const QString& text);
    void setFormatMode(int mode);
    void updateZoomBadge(qreal factor);

private:
    void setupUi();
    void applyTheme();

    QPixmap m_pixmap;
    QRect m_geometry;
    QString m_fullText;
    QVector<QRect> m_wordBoxes;
    QStringList m_words;
    QVector<QRect> m_lineBoxes;
    QStringList m_lines;

    OcrCanvas* m_canvas{ nullptr };
    QPlainTextEdit* m_textEdit{ nullptr };
    QLineEdit* m_searchBox{ nullptr };
    QLabel* m_toastLabel{ nullptr };
    QLabel* m_statsBadge{ nullptr };
    QLabel* m_zoomBadge{ nullptr };
    QPushButton* m_zoomBtn{ nullptr };
    QTimer* m_toastTimer{ nullptr };
    QGraphicsDropShadowEffect* m_shadowEffect{ nullptr };

    QColor m_accentColor;
    QColor m_contrastColor;

    bool m_isDraggingWindow{ false };
    QPoint m_dragPosition;
    int m_currentFormatMode{ 0 }; // 0: Formatted, 1: Line by Line, 2: Single Line
};

#endif
