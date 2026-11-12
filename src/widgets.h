#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QIcon>
#include <QLabel>
#include <QPixmap>

class QToolButton;
class QVariantAnimation;

inline const QColor kErrorColor(0xE0, 0x4F, 0x45);

// The sites of the simple screen.
enum class Service { YouTube, Spotify, Instagram, TikTok };

QString serviceName(Service service);
// Logo-like mark of the site, painted in code so no image plugin is needed.
QPixmap servicePixmap(Service service, int size, qreal devicePixelRatio);
QIcon serviceIcon(Service service);
// Mark of the conversion screen: a stylised F (placeholder until there is a logo).
QPixmap ffmpegPixmap(int size, qreal devicePixelRatio);
QIcon ffmpegIcon();
// The big buttons of the window header: icon above the name, one checked at a time.
QToolButton *headerButton(const QIcon &icon, const QString &text);
// Shows a file in the file manager; without a file, opens the folder.
void revealFile(const QString &file, const QString &directory);

// Small line icons that follow the text colour (light and dark mode).
enum class Glyph { Download, Stop, Trash, Retry, Folder };
QIcon glyphIcon(Glyph glyph, const QColor &color);

// On/off switch.
class ToggleSwitch : public QAbstractButton
{
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVariantAnimation *m_animation;
    qreal m_position = 0; // 0 = off, 1 = on
};

// One line of plain text, shortened with "…" when it does not fit.
class ElidedLabel : public QLabel
{
    Q_OBJECT

public:
    using QLabel::QLabel;
    void setFullText(const QString &text);
    // File names keep both ends: "a long na…me.mp3".
    void setElideMode(Qt::TextElideMode mode) { m_mode = mode; }
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_text;
    Qt::TextElideMode m_mode = Qt::ElideRight;
};
