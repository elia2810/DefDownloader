#pragma once

#include <QAbstractButton>
#include <QIcon>
#include <QLabel>
#include <QPixmap>

class QVariantAnimation;

// The sites of the simple screen.
enum class Service { YouTube, Spotify, Instagram, TikTok };

QString serviceName(Service service);
// Logo-like mark of the site, painted in code so no image plugin is needed.
QPixmap servicePixmap(Service service, int size, qreal devicePixelRatio);
QIcon serviceIcon(Service service);

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
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_text;
};
