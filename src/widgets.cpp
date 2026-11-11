#include "widgets.h"

#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

QString serviceName(Service service)
{
    switch (service) {
    case Service::YouTube: return QStringLiteral("YouTube");
    case Service::Spotify: return QStringLiteral("Spotify");
    case Service::Instagram: return QStringLiteral("Instagram");
    case Service::TikTok: return QStringLiteral("TikTok");
    }
    return {};
}

// All marks are drawn in a 1x1 square scaled to the requested size.
QPixmap servicePixmap(Service service, int size, qreal devicePixelRatio)
{
    QPixmap pixmap(QSize(size, size) * devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size, size);
    p.setPen(Qt::NoPen);

    switch (service) {
    case Service::YouTube: {
        p.setBrush(QColor(0xFF, 0x00, 0x33));
        p.drawRoundedRect(QRectF(0.02, 0.16, 0.96, 0.68), 0.2, 0.2);
        QPainterPath play;
        play.moveTo(0.40, 0.34);
        play.lineTo(0.68, 0.50);
        play.lineTo(0.40, 0.66);
        play.closeSubpath();
        p.setBrush(Qt::white);
        p.drawPath(play);
        break;
    }
    case Service::Spotify: {
        p.setBrush(QColor(0x1E, 0xD7, 0x60));
        p.drawEllipse(QRectF(0.03, 0.03, 0.94, 0.94));
        p.setBrush(Qt::NoBrush);
        const auto wave = [&p](qreal y, qreal halfWidth, qreal thickness) {
            QPainterPath path;
            path.moveTo(0.5 - halfWidth, y);
            path.quadTo(0.5, y - halfWidth * 0.42, 0.5 + halfWidth, y + halfWidth * 0.16);
            p.setPen(QPen(QColor(0x12, 0x12, 0x12), thickness, Qt::SolidLine, Qt::RoundCap));
            p.drawPath(path);
        };
        wave(0.40, 0.29, 0.085);
        wave(0.55, 0.24, 0.07);
        wave(0.69, 0.19, 0.058);
        break;
    }
    case Service::Instagram: {
        QLinearGradient gradient(0.1, 0.95, 0.9, 0.05);
        gradient.setColorAt(0.0, QColor(0xFE, 0xDA, 0x75));
        gradient.setColorAt(0.3, QColor(0xFA, 0x7E, 0x1E));
        gradient.setColorAt(0.6, QColor(0xD6, 0x29, 0x76));
        gradient.setColorAt(1.0, QColor(0x4F, 0x5B, 0xD5));
        p.setBrush(gradient);
        p.drawRoundedRect(QRectF(0.03, 0.03, 0.94, 0.94), 0.25, 0.25);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 0.07));
        p.drawRoundedRect(QRectF(0.22, 0.22, 0.56, 0.56), 0.15, 0.15);
        p.drawEllipse(QPointF(0.5, 0.5), 0.135, 0.135);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(0.665, 0.335), 0.042, 0.042);
        break;
    }
    case Service::TikTok: {
        p.setBrush(QColor(0x11, 0x11, 0x11));
        p.drawRoundedRect(QRectF(0.03, 0.03, 0.94, 0.94), 0.24, 0.24);
        // A musical note: ring, stem and hook.
        QPainterPath note;
        note.moveTo(0.555, 0.22);
        note.lineTo(0.555, 0.625);
        note.arcTo(QRectF(0.305, 0.50, 0.25, 0.25), 0, -270);
        QPainterPath hook;
        hook.moveTo(0.555, 0.22);
        hook.quadTo(0.59, 0.39, 0.76, 0.40);
        p.setBrush(Qt::NoBrush);
        const auto draw = [&](const QColor &color, qreal offset) {
            p.setPen(QPen(color, 0.105, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
            p.drawPath(note.translated(offset, offset));
            p.drawPath(hook.translated(offset, offset));
        };
        draw(QColor(0x25, 0xF4, 0xEE), -0.022);
        draw(QColor(0xFE, 0x2C, 0x55), 0.022);
        draw(Qt::white, 0);
        break;
    }
    }
    return pixmap;
}

QIcon serviceIcon(Service service)
{
    QIcon icon;
    for (int size : {24, 32, 48}) {
        icon.addPixmap(servicePixmap(service, size, 1));
        icon.addPixmap(servicePixmap(service, size, 2));
    }
    return icon;
}

QIcon glyphIcon(Glyph glyph, const QColor &color)
{
    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(16, 16) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);

        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);

        QPainterPath path;
        switch (glyph) {
        case Glyph::Download:
            path.moveTo(8, 2.5);
            path.lineTo(8, 10.5);
            path.moveTo(4.5, 7.3);
            path.lineTo(8, 10.8);
            path.lineTo(11.5, 7.3);
            path.moveTo(3, 13.5);
            path.lineTo(13, 13.5);
            break;
        case Glyph::Stop:
            p.setBrush(color);
            path.addRoundedRect(QRectF(4.5, 4.5, 7, 7), 1.2, 1.2);
            break;
        case Glyph::Trash:
            path.moveTo(2.8, 4.5);
            path.lineTo(13.2, 4.5);
            path.moveTo(6.3, 4.3);
            path.lineTo(6.3, 2.8);
            path.lineTo(9.7, 2.8);
            path.lineTo(9.7, 4.3);
            path.moveTo(4.2, 4.7);
            path.lineTo(4.9, 13.4);
            path.lineTo(11.1, 13.4);
            path.lineTo(11.8, 4.7);
            path.moveTo(6.9, 7);
            path.lineTo(6.9, 11);
            path.moveTo(9.1, 7);
            path.lineTo(9.1, 11);
            break;
        case Glyph::Retry:
            path.arcMoveTo(QRectF(3, 3, 10, 10), 60);
            path.arcTo(QRectF(3, 3, 10, 10), 60, 290);
            path.moveTo(8.2, 1.4);
            path.lineTo(10.6, 3.6);
            path.lineTo(7.6, 4.9);
            break;
        case Glyph::Folder:
            path.moveTo(2.5, 12.5);
            path.lineTo(2.5, 3.8);
            path.lineTo(6.3, 3.8);
            path.lineTo(7.7, 5.5);
            path.lineTo(13.5, 5.5);
            path.lineTo(13.5, 12.5);
            path.closeSubpath();
            break;
        }
        p.drawPath(path);
        p.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// ---------------------------------------------------------------------------

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QAbstractButton(parent)
    , m_animation(new QVariantAnimation(this))
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_animation->setDuration(120);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_position = value.toReal();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        m_animation->stop();
        m_animation->setStartValue(m_position);
        m_animation->setEndValue(on ? 1.0 : 0.0);
        m_animation->start();
    });
}

QSize ToggleSwitch::sizeHint() const
{
    return {38, 22};
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    if (m_animation->state() != QAbstractAnimation::Running)
        m_position = isChecked() ? 1 : 0; // also when the state changed with signals blocked

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
    QColor off = palette().color(QPalette::WindowText);
    off.setAlphaF(0.28);
    const QColor on = palette().color(QPalette::Highlight);
    p.setBrush(off);
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    p.setOpacity(m_position);
    p.setBrush(on);
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    p.setOpacity(1);

    const qreal diameter = track.height() - 4;
    const qreal x = track.left() + 2 + m_position * (track.width() - diameter - 4);
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(x, track.top() + 2, diameter, diameter));

    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(on, 1));
        p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    }
}

// ---------------------------------------------------------------------------

void ElidedLabel::setFullText(const QString &text)
{
    m_text = text;
    setToolTip(text);
    updateGeometry();
    update();
}

QSize ElidedLabel::minimumSizeHint() const
{
    return {40, fontMetrics().height()};
}

QSize ElidedLabel::sizeHint() const
{
    return {fontMetrics().horizontalAdvance(m_text) + 2, fontMetrics().height()};
}

void ElidedLabel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setPen(palette().color(foregroundRole()));
    p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
               fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
}
