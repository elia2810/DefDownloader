#include "convert.h"

#include "tools.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStackedLayout>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

const QStringList kVideoFormats = {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
                                   QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("gif")};
const QStringList kAudioFormats = {QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"),
                                   QStringLiteral("opus"), QStringLiteral("ogg"), QStringLiteral("flac"),
                                   QStringLiteral("wav")};
// The audio formats whose size can be decided in advance.
const QStringList kSizedFormats = {QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"),
                                   QStringLiteral("opus"), QStringLiteral("ogg")};
const QStringList kMp4Like = {QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v")};
const QString kOtherFormat = QStringLiteral("?"); // the "Altro…" entry of the format menu

// Typed extensions that hold sound only: the video is left out for them.
const QStringList kOtherAudio = {QStringLiteral("wma"), QStringLiteral("aiff"), QStringLiteral("aif"),
                                 QStringLiteral("ac3"), QStringLiteral("amr"), QStringLiteral("m4b"),
                                 QStringLiteral("mka"), QStringLiteral("oga"), QStringLiteral("mp2"),
                                 QStringLiteral("au"), QStringLiteral("caf"), QStringLiteral("wv"),
                                 QStringLiteral("spx"), QStringLiteral("tta")};

QString sizeText(qint64 bytes)
{
    const bool oneDecimal = bytes >= 1000 * 1000 && bytes < 10 * 1000 * 1000; // "2,3 MB", "480 kB", "54 MB"
    return QLocale().formattedDataSize(bytes, oneDecimal ? 1 : 0, QLocale::DataSizeSIFormat);
}

// The encoder family used for an audio file with this extension; empty: ffmpeg decides.
QString familyOfExtension(const QString &ext)
{
    if (ext == QLatin1String("mp3"))
        return QStringLiteral("mp3");
    if (ext == QLatin1String("m4a") || ext == QLatin1String("aac") || ext == QLatin1String("m4b")
        || kMp4Like.contains(ext))
        return QStringLiteral("aac");
    if (ext == QLatin1String("opus") || ext == QLatin1String("ogg") || ext == QLatin1String("webm"))
        return QStringLiteral("opus");
    return {};
}

QString familyOfCodec(const QString &codec)
{
    if (codec == QLatin1String("mp3") || codec == QLatin1String("aac") || codec == QLatin1String("opus"))
        return codec;
    return {};
}

int floorTo(const QList<int> &steps, double value)
{
    int result = steps.first();
    for (int step : steps) {
        if (step <= value)
            result = step;
    }
    return result;
}

int ceilTo(const QList<int> &steps, double value)
{
    for (int step : steps) {
        if (step >= value)
            return step;
    }
    return steps.last();
}

// loudnorm works at 192 kHz: the result is brought back to a normal rate.
int usualRate(int sourceRate)
{
    static const QList<int> rates = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    return rates.contains(sourceRate) ? sourceRate : 48000;
}

struct Audio {
    QStringList args;
    QString text;
    int kbps = 0;
    bool average = false; // mp3: the bitrate is an average, not a fixed step
    bool tooLong = false; // even the lowest bitrate does not fit
};

// Encoder settings for mp3, aac or opus. limitKbps > 0: the bitrate the result must
// stay under; when that is what decides, channels and sample rate come down with it
// (a low bitrate sounds better in mono and with fewer samples).
Audio lossyAudio(const QString &family, bool adts, const MediaInfo &info, const ConvertOptions &o, double limitKbps,
                 int maxKbps)
{
    Audio audio;
    const bool opus = family == QLatin1String("opus");
    const bool mp3 = family == QLatin1String("mp3");
    bool mono = o.mono || info.channels == 1;

    int top = mono ? (opus ? 64 : 96) : (opus ? 128 : 192);
    // Re-encoding with the same codec: more than the original has is wasted.
    if (info.audioKbps > 0 && familyOfCodec(info.audioCodec) == family)
        top = qMin(top, qMax(info.audioKbps, 32));
    const bool bound = limitKbps > 0 && (limitKbps < top || maxKbps > 0);
    if (maxKbps > 0)
        top = qMin(top, maxKbps);
    double want = limitKbps > 0 ? qMin(double(top), limitKbps) : top;

    int rate = 0; // 0: as the source
    if (mp3) {
        // The bitrates MPEG audio allows at each sample rate.
        static const QList<int> full = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192};
        if (!bound) {
            audio.kbps = ceilTo(full, want);
            if (info.sampleRate > 0 && info.sampleRate < 32000)
                audio.kbps = qMin(audio.kbps, 160);
        } else {
            mono = mono || want < 96;
            if (want >= 56) {
                audio.kbps = floorTo(full, want);
                rate = info.sampleRate == 32000 || info.sampleRate == 48000 ? 0 : 44100;
            } else if (want >= 32) {
                audio.kbps = floorTo({32, 40, 48}, want);
                rate = 24000;
            } else {
                // Down here the fixed steps are far apart (8, 16, 24): an average
                // bitrate uses the room that is left between them.
                audio.average = true;
                audio.kbps = qMax(8, int(want));
                rate = want >= 20 ? 16000 : want >= 12 ? 12000 : 8000;
                audio.tooLong = want < 8;
            }
        }
    } else if (!opus) {
        if (bound) {
            mono = mono || want < 80;
            rate = want >= 40 ? 0 : want >= 24 ? 24000 : want >= 12 ? 16000 : 8000;
            // The container spends a few bytes on every frame of 1024 samples.
            const double frames = (rate > 0 ? rate : info.sampleRate > 0 ? info.sampleRate : 44100) / 1024.0;
            want -= frames * (adts ? 7.0 : 4.5) * 8 / 1000;
            audio.tooLong = want < 8;
        }
        audio.kbps = qMax(8, int(want));
    } else {
        if (bound) {
            mono = mono || want < 40;
            want -= 0.6; // Ogg pages
            audio.tooLong = want < 6;
        }
        audio.kbps = qMax(6, int(want));
    }
    if (o.normalize && rate == 0)
        rate = opus ? 48000 : usualRate(info.sampleRate);

    if (mono && info.channels != 1)
        audio.args << QStringLiteral("-ac") << QStringLiteral("1");
    if (rate > 0)
        audio.args << QStringLiteral("-ar") << QString::number(rate);
    audio.args << QStringLiteral("-c:a")
               << (mp3 ? QStringLiteral("libmp3lame") : opus ? QStringLiteral("libopus") : QStringLiteral("aac"));
    if (audio.average)
        audio.args << QStringLiteral("-abr") << QStringLiteral("1");
    audio.args << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(audio.kbps);
    audio.text = QStringLiteral("%1 %2 kbps%3")
                     .arg(mp3 ? QStringLiteral("MP3") : opus ? QStringLiteral("Opus") : QStringLiteral("AAC"))
                     .arg(audio.kbps)
                     .arg(mono ? QStringLiteral(" mono") : QString());
    return audio;
}

// The order matters: clean first, then shape, and the volume last.
QString audioFilters(const ConvertOptions &o, bool trimSilence)
{
    QStringList chain;
    if (o.voice)
        chain << QStringLiteral("highpass=f=90");
    else if (o.denoise)
        chain << QStringLiteral("highpass=f=70");
    if (o.denoise)
        chain << QStringLiteral("afftdn=nr=12:nf=-40:tn=1");
    if (o.voice) {
        chain << QStringLiteral("equalizer=f=250:t=q:w=1:g=-2") << QStringLiteral("equalizer=f=3200:t=q:w=1.2:g=4")
              << QStringLiteral("acompressor=threshold=-21dB:ratio=3:attack=5:release=120:makeup=2");
    }
    if (trimSilence) {
        chain << QStringLiteral("silenceremove=start_periods=1:start_threshold=-45dB"
                                ":stop_periods=-1:stop_duration=0.8:stop_threshold=-45dB");
    }
    if (o.normalize)
        chain << QStringLiteral("loudnorm=I=-16:TP=-1.5:LRA=11");
    return chain.join(QLatin1Char(','));
}

QString describe(const MediaInfo &info)
{
    QStringList parts;
    parts << (info.hasVideo ? ConvertPage::tr("Video") : ConvertPage::tr("Audio"));
    const QString duration = formatDuration(int(std::lround(info.duration)));
    if (!duration.isEmpty())
        parts << duration;
    if (info.hasVideo) {
        if (info.width > 0 && info.height > 0)
            parts << QStringLiteral("%1×%2").arg(info.width).arg(info.height);
        parts << (info.hasAudio ? info.videoCodec + QStringLiteral(" + ") + info.audioCodec
                                : info.videoCodec + ConvertPage::tr(", senza audio"));
    } else {
        parts << (info.audioKbps > 0 ? QStringLiteral("%1 %2 kbps").arg(info.audioCodec).arg(info.audioKbps)
                                     : info.audioCodec);
        if (info.channels > 0)
            parts << (info.channels == 1 ? ConvertPage::tr("mono")
                      : info.channels == 2 ? ConvertPage::tr("stereo")
                                           : ConvertPage::tr("%1 canali").arg(info.channels));
    }
    if (info.size > 0)
        parts << sizeText(info.size);
    return parts.join(QStringLiteral("  ·  "));
}

QLabel *rowLabel(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

} // namespace

// ---------------------------------------------------------------------------
// FileRow

FileRow::FileRow(const QString &name, const QString &path, QWidget *parent)
    : QFrame(parent)
{
    m_title = new ElidedLabel;
    m_title->setElideMode(Qt::ElideMiddle);
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_title->setFullText(name);
    m_title->setToolTip(path);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    m_details = new ElidedLabel;
    m_details->setFont(small);
    m_bar = new QProgressBar;
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(8);
    m_bar->setRange(0, 1000);
    m_status = new ElidedLabel;
    m_status->setFont(small);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setFixedWidth(130);

    auto *progress = new QHBoxLayout;
    progress->setSpacing(8);
    progress->addWidget(m_bar, 1);
    progress->addWidget(m_status);
    auto *middle = new QVBoxLayout;
    middle->setSpacing(3);
    middle->addWidget(m_title);
    middle->addWidget(m_details);
    middle->addLayout(progress);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 2, 9);
    root->setSpacing(6);
    root->addLayout(middle, 1);
    const auto button = [root](const QString &tip) {
        auto *b = new QToolButton;
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(28, 28);
        b->setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 6px; }"
                                        "QToolButton:hover { background: rgba(128, 128, 128, 0.25); }"
                                        "QToolButton:pressed { background: rgba(128, 128, 128, 0.4); }"));
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        root->addWidget(b, 0, Qt::AlignVCenter);
        return b;
    };
    m_revealButton = button(tr("Mostra nella cartella"));
    m_removeButton = button(tr("Togli dalla lista"));
    // Hidden, the button keeps its place: the bars of all the rows end together.
    QSizePolicy keep = m_revealButton->sizePolicy();
    keep.setRetainSizeWhenHidden(true);
    m_revealButton->setSizePolicy(keep);
    m_revealButton->hide();
    connect(m_revealButton, &QToolButton::clicked, this, &FileRow::revealRequested);
    connect(m_removeButton, &QToolButton::clicked, this, &FileRow::removeRequested);
    refreshIcons();
}

void FileRow::setDetails(const QString &text, bool error)
{
    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, error ? kErrorColor : pal.color(QPalette::PlaceholderText));
    m_details->setPalette(pal);
    m_details->setFullText(text);
}

void FileRow::setProgress(int permille, const QString &status)
{
    m_bar->setRange(0, permille < 0 ? 0 : 1000);
    m_bar->setValue(qMax(0, permille));
    m_status->setFullText(status);
}

void FileRow::setDone(bool done)
{
    m_revealButton->setVisible(done);
}

void FileRow::setLocked(bool locked)
{
    m_removeButton->setEnabled(!locked);
}

void FileRow::refreshIcons()
{
    const QColor color = palette().color(QPalette::WindowText);
    m_revealButton->setIcon(glyphIcon(Glyph::Folder, color));
    m_removeButton->setIcon(glyphIcon(Glyph::Trash, color));
}

void FileRow::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_removeButton)
        refreshIcons(); // the icons follow the text colour
}

void FileRow::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);
    QPainter p(this);
    QColor line = palette().color(QPalette::WindowText);
    line.setAlphaF(0.12);
    p.setPen(line);
    p.drawLine(8, height() - 1, width() - 8, height() - 1);
}

// ---------------------------------------------------------------------------
// FileDrop

FileDrop::FileDrop(QWidget *parent)
    : QFrame(parent)
{
    setMinimumHeight(150);

    // --- no file, or one: the whole box talks about it -----------------------
    m_single = new QWidget;
    m_single->setCursor(Qt::PointingHandCursor);
    auto *icon = new QLabel;
    icon->setPixmap(ffmpegPixmap(44, qApp->devicePixelRatio()));
    icon->setFixedSize(44, 44);
    m_title = new ElidedLabel;
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setElideMode(Qt::ElideMiddle);
    QFont bold = m_title->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.15);
    m_title->setFont(bold);
    m_details = new ElidedLabel;
    m_details->setAlignment(Qt::AlignCenter);
    m_button = new QPushButton;
    m_button->setAutoDefault(false);
    m_button->setCursor(Qt::PointingHandCursor);
    connect(m_button, &QPushButton::clicked, this, &FileDrop::clicked);

    auto *single = new QVBoxLayout(m_single);
    single->setContentsMargins(24, 16, 24, 16);
    single->setSpacing(8);
    single->addStretch(1);
    single->addWidget(icon, 0, Qt::AlignHCenter);
    single->addWidget(m_title);
    single->addWidget(m_details);
    single->addSpacing(4);
    single->addWidget(m_button, 0, Qt::AlignHCenter);
    single->addStretch(1);

    // --- several files: a list that scrolls ----------------------------------
    auto *list = new QWidget;
    m_countLabel = new QLabel;
    QFont countFont = m_countLabel->font();
    countFont.setBold(true);
    m_countLabel->setFont(countFont);
    auto *hint = new ElidedLabel;
    hint->setFullText(tr("convertiti uno alla volta, dall'alto in basso"));
    QPalette faded = hint->palette();
    faded.setColor(QPalette::WindowText, faded.color(QPalette::PlaceholderText));
    hint->setPalette(faded);
    m_addButton = new QPushButton(tr("Aggiungi file…"));
    m_addButton->setAutoDefault(false);
    m_addButton->setToolTip(tr("Anche trascinando altri file qui dentro"));
    m_clearButton = new QPushButton(tr("Svuota"));
    m_clearButton->setAutoDefault(false);
    m_clearButton->setToolTip(tr("Toglie tutti i file dalla lista; i file restano dove sono"));
    connect(m_addButton, &QPushButton::clicked, this, &FileDrop::addClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &FileDrop::clearClicked);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(8, 0, 2, 0);
    head->setSpacing(10);
    head->addWidget(m_countLabel);
    head->addWidget(hint, 1);
    head->addWidget(m_addButton);
    head->addWidget(m_clearButton);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *host = new QWidget;
    auto *hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    m_rows = new QVBoxLayout;
    m_rows->setSpacing(0);
    hostLayout->addLayout(m_rows);
    hostLayout->addStretch(1);
    m_scroll->setWidget(host);
    // The box paints its own background: the list lets it show through.
    host->setAutoFillBackground(false);
    m_scroll->viewport()->setAutoFillBackground(false);

    auto *listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(10, 10, 10, 8);
    listLayout->setSpacing(6);
    listLayout->addLayout(head);
    listLayout->addWidget(m_scroll, 1);

    m_stack = new QStackedLayout(this);
    m_stack->addWidget(m_single);
    m_stack->addWidget(list);

    setFile({}, {});
}

void FileDrop::setFile(const QString &name, const QString &details, bool error)
{
    const bool empty = name.isEmpty();
    m_title->setFullText(empty ? tr("Trascina qui uno o più file video o audio") : name);
    m_details->setFullText(empty ? tr("oppure sceglili dal computer") : details);
    m_button->setText(empty ? tr("Scegli file…") : tr("Cambia file…"));

    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, error ? kErrorColor : pal.color(QPalette::PlaceholderText));
    m_details->setPalette(pal);
}

void FileDrop::addRow(FileRow *row)
{
    m_rows->addWidget(row);
}

void FileDrop::removeRow(FileRow *row)
{
    m_rows->removeWidget(row);
    row->hide();
    row->deleteLater();
}

void FileDrop::setCount(int count)
{
    m_countLabel->setText(tr("%n file", nullptr, count));
    m_stack->setCurrentIndex(count > 1 ? 1 : 0);
}

void FileDrop::showRow(FileRow *row)
{
    m_scroll->ensureWidgetVisible(row, 0, 8);
}

void FileDrop::setHighlighted(bool on)
{
    if (m_highlighted == on)
        return;
    m_highlighted = on;
    update();
}

// The list stays in use while it is converted: it can be scrolled to see how
// each file is doing.
void FileDrop::setBusy(bool busy)
{
    m_busy = busy;
    m_single->setCursor(busy ? Qt::ArrowCursor : Qt::PointingHandCursor);
    for (QPushButton *button : {m_button, m_addButton, m_clearButton})
        button->setEnabled(!busy);
}

void FileDrop::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
    QColor line = palette().color(m_highlighted ? QPalette::Highlight : QPalette::WindowText);
    QColor fill = line;
    fill.setAlphaF(m_highlighted ? 0.12 : 0.04);
    if (!m_highlighted)
        line.setAlphaF(0.3);
    p.setBrush(fill);
    p.setPen(QPen(line, 1.5, Qt::DashLine));
    p.drawRoundedRect(frame, 12, 12);
}

// A click anywhere chooses the file, as long as the box is not showing the list.
void FileDrop::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_busy && m_stack->currentIndex() == 0 && event->button() == Qt::LeftButton
        && rect().contains(event->position().toPoint()))
        emit clicked();
}

// ---------------------------------------------------------------------------
// The command

MediaInfo ConvertPage::parseProbe(const QByteArray &json, const QString &path)
{
    MediaInfo info;
    info.path = path;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject format = root.value(QLatin1String("format")).toObject();
    info.duration = format.value(QLatin1String("duration")).toString().toDouble();
    info.size = format.value(QLatin1String("size")).toString().toLongLong();

    for (const QJsonValue &value : root.value(QLatin1String("streams")).toArray()) {
        const QJsonObject stream = value.toObject();
        const QString type = stream.value(QLatin1String("codec_type")).toString();
        if (type == QLatin1String("video")) {
            // The cover of a song is listed as a video with one picture.
            if (stream.value(QLatin1String("disposition")).toObject().value(QLatin1String("attached_pic")).toInt()) {
                info.hasCover = true;
            } else if (!info.hasVideo) {
                info.hasVideo = true;
                info.videoCodec = stream.value(QLatin1String("codec_name")).toString();
                info.width = stream.value(QLatin1String("width")).toInt();
                info.height = stream.value(QLatin1String("height")).toInt();
            }
        } else if (type == QLatin1String("audio") && !info.hasAudio) {
            info.hasAudio = true;
            info.audioCodec = stream.value(QLatin1String("codec_name")).toString();
            info.sampleRate = stream.value(QLatin1String("sample_rate")).toString().toInt();
            info.channels = stream.value(QLatin1String("channels")).toInt();
            info.audioKbps = qRound(stream.value(QLatin1String("bit_rate")).toString().toDouble() / 1000);
        }
    }
    // Some containers only know the bitrate of the whole file.
    if (info.hasAudio && !info.hasVideo && info.audioKbps <= 0)
        info.audioKbps = qRound(format.value(QLatin1String("bit_rate")).toString().toDouble() / 1000);
    info.valid = info.hasVideo || info.hasAudio;
    return info;
}

ConvertPlan ConvertPage::plan(const MediaInfo &info, const ConvertOptions &o, int maxKbps)
{
    ConvertPlan plan;
    if (!info.valid) {
        plan.error = tr("Scegli un file per cominciare");
        plan.nothingToDo = true;
        return plan;
    }
    const QString inputExt = QFileInfo(info.path).suffix().toLower();
    const QString target = o.format;

    // What comes out: a compressed result is always an audio file.
    if (o.limitMb > 0) {
        plan.audioOnly = true;
        if (kSizedFormats.contains(target))
            plan.extension = target;
        else if (target.isEmpty() && !info.hasVideo && kSizedFormats.contains(inputExt))
            plan.extension = inputExt;
        else
            plan.extension = QStringLiteral("mp3");
    } else {
        plan.extension = target.isEmpty() ? inputExt : target;
        plan.audioOnly = !info.hasVideo || kAudioFormats.contains(plan.extension)
                         || (!target.isEmpty() && kOtherAudio.contains(plan.extension));
    }
    const QString ext = plan.extension;
    const bool trim = o.trimSilence && plan.audioOnly; // a video would lose sync
    const bool filters = o.denoise || o.voice || o.normalize || trim || o.mono;

    if (target.isEmpty() && o.limitMb == 0 && !filters) {
        plan.error = tr("Scegli un formato, una compressione o un miglioramento dell'audio");
        plan.nothingToDo = true;
        return plan;
    }
    if (ext.isEmpty()) {
        plan.error = tr("Scegli il formato del risultato");
        return plan;
    }

    QStringList &args = plan.arguments;
    if (ext == QLatin1String("gif")) {
        if (!info.hasVideo) {
            plan.error = tr("Per una GIF serve un video");
            return plan;
        }
        args << QStringLiteral("-vf")
             << QStringLiteral("fps=12,scale='min(480,iw)':-2:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse")
             << QStringLiteral("-an");
        plan.details = tr("GIF animata, 12 immagini al secondo, senza audio");
        return plan;
    }
    if (!info.hasAudio && (plan.audioOnly || filters)) {
        plan.error = tr("Questo file non ha una traccia audio");
        return plan;
    }

    // Words added to the name of the result, in the language of the interface.
    if (o.denoise)
        plan.tags << tr("pulito");
    if (o.voice)
        plan.tags << tr("voce");
    if (o.normalize)
        plan.tags << tr("volume");
    if (trim)
        plan.tags << tr("nosilenzi");
    if (o.mono)
        plan.tags << tr("mono");
    if (o.limitMb > 0)
        plan.tags << QStringLiteral("whatsapp%1").arg(o.limitMb);
    if (plan.tags.isEmpty() && ext == inputExt)
        plan.tags << tr("convertito");

    const QString chain = audioFilters(o, trim);
    const QStringList monoArgs = o.mono && info.channels != 1 ? QStringList{QStringLiteral("-ac"), QStringLiteral("1")}
                                                              : QStringList();
    const QStringList rateArgs = o.normalize ? QStringList{QStringLiteral("-ar"),
                                                           QString::number(usualRate(info.sampleRate))}
                                             : QStringList();

    if (plan.audioOnly) {
        static const QStringList withCover = {QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("flac")};
        if (info.hasCover && !info.hasVideo && o.limitMb == 0 && withCover.contains(ext)) {
            args << QStringLiteral("-map") << QStringLiteral("0:a:0") << QStringLiteral("-map")
                 << QStringLiteral("0:v:0") << QStringLiteral("-c:v") << QStringLiteral("copy")
                 << QStringLiteral("-disposition:v:0") << QStringLiteral("attached_pic");
        } else {
            args << QStringLiteral("-vn");
        }
        args << QStringLiteral("-sn");
        if (!chain.isEmpty())
            args << QStringLiteral("-af") << chain;

        const QString family = familyOfExtension(ext);
        if (!family.isEmpty()) {
            double limitKbps = 0;
            if (o.limitMb > 0) {
                if (info.duration <= 0) {
                    plan.error = tr("La durata del file non è nota: non si può calcolare la compressione");
                    return plan;
                }
                // Megabytes of 1000 x 1000 bytes, the stricter reading, less a small margin.
                limitKbps = o.limitMb * 1000.0 * 1000.0 * 0.97 * 8 / info.duration / 1000;
            }
            const Audio audio = lossyAudio(family, ext == QLatin1String("aac"), info, o, limitKbps, maxKbps);
            if (audio.tooLong) {
                plan.error = tr("Il file è troppo lungo per stare in %1 MB").arg(o.limitMb);
                return plan;
            }
            args << audio.args;
            plan.kbps = audio.kbps;
            plan.details = audio.text;
            if (info.duration > 0)
                plan.estimatedBytes = qint64(audio.kbps * 125.0 * info.duration);
        } else {
            args << monoArgs << rateArgs;
            if (ext == QLatin1String("flac")) {
                args << QStringLiteral("-c:a") << QStringLiteral("flac");
                plan.details = tr("FLAC, senza perdita di qualità");
            } else if (ext == QLatin1String("wav")) {
                args << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le");
                plan.details = tr("WAV, senza compressione");
            } else {
                plan.details = tr("audio %1").arg(ext);
            }
        }
        if (ext == QLatin1String("mp3"))
            args << QStringLiteral("-id3v2_version") << QStringLiteral("3"); // read by every player
        if (ext == QLatin1String("m4a") || ext == QLatin1String("m4b") || kMp4Like.contains(ext))
            args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
        return plan;
    }

    // --- a video stays a video ---------------------------------------------
    const bool same = ext == inputExt;
    const bool mp4Like = kMp4Like.contains(ext);
    const bool mkv = ext == QLatin1String("mkv");
    const bool webm = ext == QLatin1String("webm");
    const bool avi = ext == QLatin1String("avi");
    QStringList details;

    if (same) {
        args << QStringLiteral("-map") << QStringLiteral("0:V") << QStringLiteral("-map") << QStringLiteral("0:a?")
             << QStringLiteral("-map") << QStringLiteral("0:s?") << QStringLiteral("-c:s") << QStringLiteral("copy");
    } else {
        args << QStringLiteral("-sn"); // subtitles rarely survive a change of container
    }

    // Streams the new container can hold are copied as they are: fast and lossless.
    const QStringList videoFits = mp4Like ? QStringList{QStringLiteral("h264"), QStringLiteral("hevc"),
                                                        QStringLiteral("av1"), QStringLiteral("mpeg4")}
                                  : webm ? QStringList{QStringLiteral("vp8"), QStringLiteral("vp9"), QStringLiteral("av1")}
                                  : avi ? QStringList{QStringLiteral("mpeg4")}
                                        : QStringList();
    if (same || mkv || videoFits.contains(info.videoCodec)) {
        args << QStringLiteral("-c:v") << QStringLiteral("copy");
        if (mp4Like && info.videoCodec == QLatin1String("hevc"))
            args << QStringLiteral("-tag:v") << QStringLiteral("hvc1"); // or Apple players refuse it
        details << tr("video copiato senza ricodifica");
    } else if (mp4Like) {
        args << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset")
             << QStringLiteral("medium") << QStringLiteral("-crf") << QStringLiteral("20")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
        if (info.width % 2 || info.height % 2) // H.264 wants even sides
            args << QStringLiteral("-vf") << QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2");
        details << tr("video H.264");
    } else if (webm) {
        args << QStringLiteral("-c:v") << QStringLiteral("libvpx-vp9") << QStringLiteral("-crf")
             << QStringLiteral("32") << QStringLiteral("-b:v") << QStringLiteral("0") << QStringLiteral("-row-mt")
             << QStringLiteral("1");
        details << tr("video VP9");
    } else if (avi) {
        args << QStringLiteral("-c:v") << QStringLiteral("mpeg4") << QStringLiteral("-q:v") << QStringLiteral("4");
        details << tr("video MPEG-4");
    } else {
        details << tr("video %1").arg(ext);
    }

    if (info.hasAudio) {
        const QStringList audioFits = mp4Like ? QStringList{QStringLiteral("aac"), QStringLiteral("mp3"),
                                                            QStringLiteral("ac3"), QStringLiteral("eac3"),
                                                            QStringLiteral("alac")}
                                      : webm ? QStringList{QStringLiteral("opus"), QStringLiteral("vorbis")}
                                      : avi ? QStringList{QStringLiteral("mp3"), QStringLiteral("ac3")}
                                            : QStringList();
        if (!filters && (same || mkv || audioFits.contains(info.audioCodec))) {
            args << QStringLiteral("-c:a") << QStringLiteral("copy");
            details << tr("audio copiato");
        } else {
            if (!chain.isEmpty())
                args << QStringLiteral("-af") << chain;
            // The same container keeps its kind of audio; one this screen does
            // not know is left to ffmpeg.
            QString family = mp4Like ? QStringLiteral("aac")
                             : webm ? QStringLiteral("opus")
                             : avi ? QStringLiteral("mp3")
                             : same || mkv ? familyOfCodec(info.audioCodec)
                                           : QString();
            if (family.isEmpty() && mkv)
                family = QStringLiteral("aac");
            if (!family.isEmpty()) {
                const Audio audio = lossyAudio(family, false, info, o, 0, 0);
                args << audio.args;
                details << tr("audio %1").arg(audio.text);
            } else {
                args << monoArgs << rateArgs;
                details << tr("audio ricodificato");
            }
        }
    }
    if (mp4Like)
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    plan.details = details.join(QStringLiteral(", "));
    return plan;
}

QString ConvertPage::outputPath(const QString &input, const QString &directory, const ConvertPlan &plan)
{
    const QFileInfo source(input);
    const QDir dir(directory.isEmpty() ? source.absolutePath() : directory);
    QString base = source.completeBaseName();
    if (!plan.tags.isEmpty())
        base += QLatin1Char('_') + plan.tags.join(QLatin1Char('_'));
    QString path = dir.filePath(base + QLatin1Char('.') + plan.extension);
    for (int n = 2; QFileInfo::exists(path); ++n) // nothing is ever overwritten
        path = dir.filePath(QStringLiteral("%1 (%2).%3").arg(base).arg(n).arg(plan.extension));
    return path;
}

// ---------------------------------------------------------------------------
// ConvertPage

// One file of the list.
struct ConvertItem {
    enum State { Probing, Unreadable, Waiting, Queued, Running, Done, Failed, Stopped };
    MediaInfo info;  // only the path, until ffprobe has read the file
    State state = Probing;
    QString error;   // why it cannot be read, or why its conversion failed
    QString output;  // the file written, once Done
    QProcess *probe = nullptr;
    FileRow *row = nullptr;
};

ConvertPage::ConvertPage(QWidget *parent)
    : QWidget(parent)
{
    setAcceptDrops(true);
    QSettings settings;

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 6, 12, 12);
    root->setSpacing(12);

    m_drop = new FileDrop;
    root->addWidget(m_drop, 1);
    connect(m_drop, &FileDrop::clicked, this, [this] { browse(false); });
    connect(m_drop, &FileDrop::addClicked, this, [this] { browse(true); });
    connect(m_drop, &FileDrop::clearClicked, this, &ConvertPage::clearItems);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);
    root->addLayout(grid);

    // --- format ------------------------------------------------------------
    m_format = new QComboBox;
    m_format->setMinimumWidth(250);
    m_format->addItem(tr("Nessuna conversione (stesso formato)"), QString());
    m_format->insertSeparator(m_format->count());
    for (const QString &format : kVideoFormats) {
        m_format->addItem(format == QLatin1String("gif") ? tr("gif  (animazione senza audio)")
                                                         : tr("%1  (video)").arg(format),
                          format);
    }
    m_format->insertSeparator(m_format->count());
    for (const QString &format : kAudioFormats)
        m_format->addItem(tr("%1  (audio)").arg(format), format);
    m_format->insertSeparator(m_format->count());
    m_format->addItem(tr("Altro…"), kOtherFormat);
    m_format->setMaxVisibleItems(20);
    m_otherFormat = new QLineEdit;
    m_otherFormat->setPlaceholderText(tr("estensione, es. wmv"));
    m_otherFormat->setMaximumWidth(170);
    m_otherFormat->setToolTip(tr("Qualsiasi formato che ffmpeg sa scrivere: basta la sua estensione"));
    auto *formatRow = new QHBoxLayout;
    formatRow->addWidget(m_format);
    formatRow->addWidget(m_otherFormat);
    formatRow->addStretch(1);
    grid->addWidget(rowLabel(tr("Converti in")), 0, 0);
    grid->addLayout(formatRow, 0, 1);

    // --- compression -------------------------------------------------------
    m_limit64 = new QCheckBox(tr("Fino a 64 MB"));
    m_limit16 = new QCheckBox(tr("Fino a 16 MB"));
    const QString limitTip = tr("Il risultato è un file audio che sta nel limite, pronto da mandare su WhatsApp.\n"
                                "Da un video viene estratto solo l'audio. La qualità è la più alta che ci sta.");
    m_limit64->setToolTip(limitTip);
    m_limit16->setToolTip(limitTip);
    auto *limitHint = new QLabel(tr("per WhatsApp: da un video resta solo l'audio"));
    QPalette faded = limitHint->palette();
    faded.setColor(QPalette::WindowText, faded.color(QPalette::PlaceholderText));
    limitHint->setPalette(faded);
    auto *limitRow = new QHBoxLayout;
    limitRow->setSpacing(18);
    limitRow->addWidget(m_limit64);
    limitRow->addWidget(m_limit16);
    limitRow->addWidget(limitHint);
    limitRow->addStretch(1);
    grid->addWidget(rowLabel(tr("Comprimi l'audio")), 1, 0);
    grid->addLayout(limitRow, 1, 1);

    // --- audio filters -----------------------------------------------------
    m_denoise = new QCheckBox(tr("Riduci il rumore di fondo"));
    m_denoise->setToolTip(tr("Attenua fruscio, ventole e ronzii costanti"));
    m_voice = new QCheckBox(tr("Esalta la voce"));
    m_voice->setToolTip(tr("Più chiarezza al parlato, e meno distanza tra le parole forti e quelle deboli"));
    m_normalize = new QCheckBox(tr("Uniforma il volume"));
    m_normalize->setToolTip(tr("Porta l'audio a un volume standard: né troppo basso né troppo alto"));
    m_trimSilence = new QCheckBox(tr("Accorcia le pause di silenzio"));
    m_trimSilence->setToolTip(tr("Le pause più lunghe di 0,8 secondi vengono accorciate.\n"
                                 "Solo quando il risultato è un file audio: un video andrebbe fuori sincrono."));
    m_mono = new QCheckBox(tr("Mono"));
    m_mono->setToolTip(tr("Un solo canale: per la voce basta, e il file pesa meno"));
    auto *filters = new QGridLayout;
    filters->setHorizontalSpacing(18);
    filters->setVerticalSpacing(8);
    filters->addWidget(m_denoise, 0, 0);
    filters->addWidget(m_voice, 0, 1);
    filters->addWidget(m_normalize, 0, 2);
    filters->addWidget(m_trimSilence, 1, 0);
    filters->addWidget(m_mono, 1, 1);
    filters->setColumnStretch(3, 1);
    grid->addWidget(rowLabel(tr("Migliora l'audio")), 2, 0, Qt::AlignTop);
    grid->addLayout(filters, 2, 1);

    // --- folder ------------------------------------------------------------
    m_dirEdit = new QLineEdit(settings.value(QStringLiteral("convert/dir")).toString());
    m_dirEdit->setPlaceholderText(tr("Nella stessa cartella di ogni file"));
    m_dirEdit->setClearButtonEnabled(true);
    m_dirButton = new QPushButton(tr("Sfoglia…"));
    m_dirButton->setAutoDefault(false);
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(m_dirEdit, 1);
    dirRow->addWidget(m_dirButton);
    grid->addWidget(rowLabel(tr("Salva in")), 3, 0);
    grid->addLayout(dirRow, 3, 1);
    connect(m_dirButton, &QPushButton::clicked, this, [this] {
        const QString start = !directory().isEmpty() ? directory()
                              : m_items.isEmpty() ? QString()
                                                  : QFileInfo(m_items.first()->info.path).absolutePath();
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Cartella di destinazione"), start);
        if (!dir.isEmpty())
            m_dirEdit->setText(dir);
    });
    connect(m_dirEdit, &QLineEdit::textChanged, this, &ConvertPage::choicesChanged);

    // --- result and run ----------------------------------------------------
    m_resultLabel = new ElidedLabel;
    m_resultLabel->setElideMode(Qt::ElideMiddle);
    root->addWidget(m_resultLabel);

    m_bar = new QProgressBar;
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(8);
    m_bar->setRange(0, 1000);
    m_statusLabel = new ElidedLabel;
    m_statusLabel->setElideMode(Qt::ElideMiddle);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_revealButton = new QPushButton(tr("Mostra nella cartella"));
    m_revealButton->setAutoDefault(false);
    m_revealButton->hide();
    m_runButton = new QPushButton(tr("Converti"));
    m_runButton->setAutoDefault(false);
    m_runButton->setMinimumWidth(110);
    auto *runRow = new QHBoxLayout;
    runRow->setSpacing(10);
    runRow->addWidget(m_bar, 1);
    runRow->addWidget(m_statusLabel, 1);
    runRow->addWidget(m_revealButton);
    runRow->addWidget(m_runButton);
    root->addLayout(runRow);
    connect(m_revealButton, &QPushButton::clicked, this,
            [this] { revealFile(m_doneFile, QFileInfo(m_doneFile).absolutePath()); });
    connect(m_runButton, &QPushButton::clicked, this, [this] {
        if (m_process)
            stop();
        else
            run();
    });

    // --- saved choices -----------------------------------------------------
    m_loading = true;
    const QString savedFormat = settings.value(QStringLiteral("convert/format")).toString();
    const int known = m_format->findData(savedFormat);
    if (known >= 0) {
        m_format->setCurrentIndex(known);
    } else {
        m_format->setCurrentIndex(m_format->findData(kOtherFormat));
        m_otherFormat->setText(savedFormat);
    }
    const int limit = settings.value(QStringLiteral("convert/limit")).toInt();
    m_limit64->setChecked(limit == 64);
    m_limit16->setChecked(limit == 16);
    m_denoise->setChecked(settings.value(QStringLiteral("convert/denoise")).toBool());
    m_voice->setChecked(settings.value(QStringLiteral("convert/voice")).toBool());
    m_normalize->setChecked(settings.value(QStringLiteral("convert/normalize")).toBool());
    m_trimSilence->setChecked(settings.value(QStringLiteral("convert/trimSilence")).toBool());
    m_mono->setChecked(settings.value(QStringLiteral("convert/mono")).toBool());
    m_loading = false;

    // The two sizes exclude each other, but both can be off.
    connect(m_limit64, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            m_limit16->setChecked(false);
    });
    connect(m_limit16, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            m_limit64->setChecked(false);
    });
    connect(m_format, &QComboBox::currentIndexChanged, this, [this] {
        choicesChanged();
        if (m_otherFormat->isVisible())
            m_otherFormat->setFocus();
    });
    connect(m_otherFormat, &QLineEdit::textChanged, this, &ConvertPage::choicesChanged);
    for (QCheckBox *check : {m_limit64, m_limit16, m_denoise, m_voice, m_normalize, m_trimSilence, m_mono})
        connect(check, &QCheckBox::toggled, this, &ConvertPage::choicesChanged);

    connect(qApp, &QCoreApplication::aboutToQuit, this, &ConvertPage::shutDown);
    refresh();
}

ConvertPage::~ConvertPage()
{
    shutDown();
    qDeleteAll(m_items);
}

// A conversion cut short leaves half a file: it is stopped and removed.
void ConvertPage::shutDown()
{
    for (ConvertItem *item : std::as_const(m_items)) {
        if (!item->probe)
            continue;
        item->probe->disconnect();
        item->probe->kill();
        item->probe->waitForFinished(500);
        item->probe = nullptr;
    }
    m_probing = 0;
    if (!m_process)
        return;
    m_process->disconnect();
    stopTool(m_process);
    m_process->waitForFinished(1500);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    m_process = nullptr;
    QFile::remove(m_output);
}

ConvertOptions ConvertPage::options() const
{
    ConvertOptions o;
    o.format = m_format->currentData().toString();
    if (o.format == kOtherFormat) {
        static const QRegularExpression notAllowed(QStringLiteral("[^a-z0-9]"));
        o.format = m_otherFormat->text().toLower().remove(notAllowed);
    }
    o.limitMb = m_limit64->isChecked() ? 64 : m_limit16->isChecked() ? 16 : 0;
    o.denoise = m_denoise->isChecked();
    o.voice = m_voice->isChecked();
    o.normalize = m_normalize->isChecked();
    o.trimSilence = m_trimSilence->isChecked();
    o.mono = m_mono->isChecked();
    return o;
}

QString ConvertPage::directory() const
{
    const QString dir = m_dirEdit->text().trimmed();
    if (dir == QLatin1String("~") || dir.startsWith(QLatin1String("~/")))
        return QDir::homePath() + dir.mid(1);
    return dir;
}

void ConvertPage::saveOptions()
{
    if (m_loading)
        return;
    const ConvertOptions o = options();
    QSettings settings;
    settings.setValue(QStringLiteral("convert/format"), o.format);
    settings.setValue(QStringLiteral("convert/limit"), o.limitMb);
    settings.setValue(QStringLiteral("convert/denoise"), o.denoise);
    settings.setValue(QStringLiteral("convert/voice"), o.voice);
    settings.setValue(QStringLiteral("convert/normalize"), o.normalize);
    settings.setValue(QStringLiteral("convert/trimSilence"), o.trimSilence);
    settings.setValue(QStringLiteral("convert/mono"), o.mono);
    settings.setValue(QStringLiteral("convert/dir"), m_dirEdit->text().trimmed());
}

// ---------------------------------------------------------------------------
// The list of files

// add: the chosen files join the list instead of taking its place.
void ConvertPage::browse(bool add)
{
    if (m_process)
        return;
    QSettings settings;
    const QString start = m_items.isEmpty() ? settings.value(QStringLiteral("convert/lastDir"), downloadsDir()).toString()
                                            : QFileInfo(m_items.last()->info.path).absolutePath();
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Scegli uno o più file video o audio"), start,
        tr("Video e audio (*.mp4 *.mkv *.mov *.webm *.avi *.m4v *.wmv *.flv *.ts *.mpg *.mpeg *.3gp *.gif "
           "*.mp3 *.m4a *.aac *.opus *.ogg *.oga *.flac *.wav *.wma *.aiff *.aif *.amr *.m4b *.mka);;Tutti i file (*)"));
    if (files.isEmpty())
        return;
    if (add)
        addInputs(files);
    else
        setInputs(files);
}

void ConvertPage::setInputs(const QStringList &paths)
{
    if (m_process)
        return;
    clearItems();
    addInputs(paths);
}

void ConvertPage::addInputs(const QStringList &paths)
{
    if (m_process || paths.isEmpty())
        return;
    resetStatus();
    const bool canProbe = !findTool(QStringLiteral("ffprobe")).isEmpty();
    QString lastDir;
    for (const QString &path : paths) {
        const QFileInfo file(path);
        const QString absolute = file.absoluteFilePath();
        if (std::any_of(m_items.cbegin(), m_items.cend(),
                        [&absolute](const ConvertItem *item) { return item->info.path == absolute; }))
            continue;
        auto *item = new ConvertItem;
        item->info.path = absolute;
        if (!file.isFile()) {
            item->state = ConvertItem::Unreadable;
            item->error = tr("Non è un file");
        } else if (!canProbe) {
            item->state = ConvertItem::Unreadable;
            item->error = tr("ffprobe non trovato");
        } else {
            lastDir = file.absolutePath();
        }
        item->row = new FileRow(file.fileName(), QDir::toNativeSeparators(absolute));
        connect(item->row, &FileRow::removeRequested, this, [this, item] { removeItem(item); });
        connect(item->row, &FileRow::revealRequested, this,
                [item] { revealFile(item->output, QFileInfo(item->output).absolutePath()); });
        m_drop->addRow(item->row);
        m_items.append(item);
    }
    if (!lastDir.isEmpty())
        QSettings().setValue(QStringLiteral("convert/lastDir"), lastDir);
    m_drop->setCount(m_items.size());
    probeNext();
    refresh();
}

void ConvertPage::removeItem(ConvertItem *item)
{
    if (m_process || !m_items.removeOne(item))
        return;
    if (item->probe) {
        item->probe->disconnect();
        item->probe->kill();
        item->probe->deleteLater();
        --m_probing;
    }
    m_drop->removeRow(item->row);
    delete item;
    m_drop->setCount(m_items.size());
    if (m_items.isEmpty())
        resetStatus();
    probeNext();
    refresh();
}

void ConvertPage::clearItems()
{
    if (m_process)
        return;
    for (ConvertItem *item : std::as_const(m_items)) {
        if (item->probe) {
            item->probe->disconnect();
            item->probe->kill();
            item->probe->deleteLater();
        }
        m_drop->removeRow(item->row);
        delete item;
    }
    m_items.clear();
    m_probing = 0;
    m_drop->setCount(0);
    resetStatus();
    refresh();
}

// ffprobe reads the files a few at a time: a long list does not start a
// hundred programs together.
void ConvertPage::probeNext()
{
    static const int atOnce = 3;
    if (m_probing >= atOnce)
        return;
    const QString program = findTool(QStringLiteral("ffprobe"));
    for (ConvertItem *item : std::as_const(m_items)) {
        if (m_probing >= atOnce)
            break;
        if (item->state != ConvertItem::Probing || item->probe)
            continue;
        auto *probe = new QProcess(this);
        item->probe = probe;
        ++m_probing;
        probe->setProcessEnvironment(toolEnvironment());
        connect(probe, &QProcess::finished, this, [this, item, probe] {
            const MediaInfo info = parseProbe(probe->readAllStandardOutput(), item->info.path);
            if (info.valid)
                item->info = info;
            endProbe(item, info.valid ? QString() : tr("Non è un file video o audio che ffmpeg sa leggere"));
        });
        connect(probe, &QProcess::errorOccurred, this, [this, item](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                endProbe(item, tr("Impossibile avviare ffprobe"));
        });
        probe->start(program, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                               QStringLiteral("format=duration,size,bit_rate:stream=codec_type,codec_name,width,height,"
                                              "bit_rate,sample_rate,channels:stream_disposition=attached_pic"),
                               QStringLiteral("-of"), QStringLiteral("json"), item->info.path});
        // A program that cannot start says so at once: the probe is already gone.
        if (item->probe == probe)
            probe->closeWriteChannel();
    }
}

void ConvertPage::endProbe(ConvertItem *item, const QString &error)
{
    if (!item->probe)
        return;
    item->probe->disconnect();
    item->probe->deleteLater();
    item->probe = nullptr;
    --m_probing;
    item->state = error.isEmpty() ? ConvertItem::Waiting : ConvertItem::Unreadable;
    item->error = error;
    // Not from inside the loop that is starting the probes.
    QMetaObject::invokeMethod(this, &ConvertPage::probeNext, Qt::QueuedConnection);
    refresh();
}

// What a row says about its file. plan: what the current choices make of it.
void ConvertPage::updateRow(ConvertItem *item, const ConvertPlan &plan)
{
    const QString source = describe(item->info);
    QString details = source;
    QString status;
    bool error = false;
    int bar = 0;
    switch (item->state) {
    case ConvertItem::Probing:
        details = tr("Lettura del file…");
        break;
    case ConvertItem::Unreadable:
    case ConvertItem::Failed:
        details = item->error;
        error = true;
        if (item->state == ConvertItem::Failed)
            status = tr("Non riuscito");
        break;
    case ConvertItem::Waiting:
        // These choices cannot be applied to this file: it is left out.
        if (!plan.error.isEmpty() && !plan.nothingToDo) {
            details = plan.error;
            error = true;
        }
        break;
    case ConvertItem::Queued:
        status = tr("In coda");
        break;
    case ConvertItem::Running:
        break;
    case ConvertItem::Done:
        details = tr("Creato:  %1").arg(QFileInfo(item->output).fileName());
        status = tr("Fatto  ·  %1").arg(sizeText(QFileInfo(item->output).size()));
        bar = 1000;
        break;
    case ConvertItem::Stopped:
        status = tr("Interrotto");
        break;
    }
    item->row->setDetails(details, error);
    if (item->state != ConvertItem::Running) // the conversion writes its own progress
        item->row->setProgress(bar, status);
    item->row->setDone(item->state == ConvertItem::Done);

    // Alone, the file fills the box: why a conversion is not possible or did
    // not work is said below, next to the button.
    if (m_items.size() == 1) {
        const bool unread = item->state == ConvertItem::Probing || item->state == ConvertItem::Unreadable;
        m_drop->setFile(QFileInfo(item->info.path).fileName(), unread ? details : source,
                        item->state == ConvertItem::Unreadable);
    }
}

// A choice was changed: what was converted with the old ones is to do again.
void ConvertPage::choicesChanged()
{
    if (!m_process) {
        for (ConvertItem *item : std::as_const(m_items)) {
            if (item->state == ConvertItem::Done || item->state == ConvertItem::Failed
                || item->state == ConvertItem::Stopped) {
                item->state = ConvertItem::Waiting;
                item->error.clear();
                item->output.clear();
            }
        }
    }
    refresh();
}

// Shows what the current choices would produce.
void ConvertPage::refresh()
{
    saveOptions();
    m_otherFormat->setVisible(m_format->currentData().toString() == kOtherFormat);
    if (m_process)
        return;

    const ConvertOptions o = options();
    QList<const ConvertItem *> ready; // the files these choices can convert
    QList<ConvertPlan> plans;         // one for each of them
    ConvertPlan refused;              // why the first of the others cannot
    bool sameRefusal = true;
    int readable = 0;
    bool audioOnly = false;
    QList<ConvertPlan> all;           // one for each file of the list
    for (const ConvertItem *item : std::as_const(m_items)) {
        if (item->state == ConvertItem::Probing || item->state == ConvertItem::Unreadable) {
            all << ConvertPlan();
            continue;
        }
        const ConvertPlan p = plan(item->info, o);
        all << p;
        ++readable;
        audioOnly = audioOnly || p.audioOnly;
        if (p.error.isEmpty()) {
            ready << item;
            plans << p;
        } else if (refused.error.isEmpty()) {
            refused = p;
        } else if (refused.error != p.error) {
            sameRefusal = false;
        }
    }
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        ConvertPlan p = all.at(i);
        // The choices change the other files and not this one (pauses to
        // shorten, and this is a video): its row says that it is left out.
        if (p.nothingToDo && !ready.isEmpty()) {
            p.nothingToDo = false;
            p.error = tr("Con queste scelte questo file resterebbe com'è");
        }
        updateRow(m_items.at(i), p);
    }
    if (m_items.isEmpty())
        m_drop->setFile({}, {});
    // Removing pauses only makes sense when no video has to stay in sync.
    m_trimSilence->setEnabled(readable == 0 || audioOnly);

    QPalette pal = palette();
    const QColor faded = pal.color(QPalette::PlaceholderText);
    const QString separator = QStringLiteral("  ·  ");
    QString text;
    if (m_items.isEmpty()) {
        pal.setColor(QPalette::WindowText, faded);
        text = plan(MediaInfo(), o).error;
    } else if (m_probing > 0 || readable == 0) {
        pal.setColor(QPalette::WindowText, faded); // the box and the rows say what is going on
    } else if (ready.isEmpty()) {
        pal.setColor(QPalette::WindowText, refused.nothingToDo ? faded : kErrorColor);
        text = sameRefusal ? refused.error : tr("Nessuno di questi file si può convertire con queste scelte");
    } else if (m_items.size() == 1) {
        const ConvertPlan &p = plans.first();
        QStringList parts = {QFileInfo(outputPath(ready.first()->info.path, directory(), p)).fileName(), p.details};
        if (p.estimatedBytes > 0)
            parts << (o.trimSilence ? tr("al massimo %1") : tr("circa %1")).arg(sizeText(p.estimatedBytes));
        text = tr("Risultato:  %1").arg(parts.join(separator));
    } else {
        // Several files. The line is about the ones the button would convert
        // now: those still to do or, when none is left, all of them again.
        QList<ConvertPlan> next;
        for (qsizetype i = 0; i < ready.size(); ++i) {
            if (ready.at(i)->state != ConvertItem::Done)
                next << plans.at(i);
        }
        if (next.isEmpty())
            next = plans;
        // What they have in common, and the size of all together.
        QStringList parts = {tr("%n file", nullptr, int(next.size()))};
        qint64 bytes = 0;
        bool sameDetails = true;
        bool sized = true;
        for (const ConvertPlan &p : std::as_const(next)) {
            sameDetails = sameDetails && p.details == next.first().details;
            sized = sized && p.estimatedBytes > 0;
            bytes += p.estimatedBytes;
        }
        if (sameDetails && !next.first().details.isEmpty())
            parts << next.first().details;
        if (sized)
            parts << (o.trimSilence ? tr("al massimo %1 in tutto") : tr("circa %1 in tutto")).arg(sizeText(bytes));
        if (ready.size() < readable)
            parts << tr("esclusi: %1").arg(readable - ready.size());
        text = tr("Risultato:  %1").arg(parts.join(separator));
    }
    m_resultLabel->setPalette(pal);
    m_resultLabel->setFullText(text);
    m_runButton->setEnabled(m_probing == 0 && !ready.isEmpty());
}

void ConvertPage::resetStatus()
{
    m_doneFile.clear();
    m_revealButton->hide();
    m_bar->setRange(0, 1000);
    m_bar->setValue(0);
    setStatus({});
}

void ConvertPage::setStatus(const QString &text, bool error)
{
    QPalette pal = palette();
    if (error)
        pal.setColor(QPalette::WindowText, kErrorColor);
    m_statusLabel->setPalette(pal);
    m_statusLabel->setFullText(text);
}

void ConvertPage::setRunning(bool running)
{
    for (QWidget *widget : std::initializer_list<QWidget *>{m_format, m_otherFormat, m_limit64, m_limit16,
                                                            m_denoise, m_voice, m_normalize, m_trimSilence, m_mono,
                                                            m_dirEdit, m_dirButton})
        widget->setEnabled(!running);
    m_drop->setBusy(running);
    for (ConvertItem *item : std::as_const(m_items))
        item->row->setLocked(running);
    m_runButton->setText(running ? tr("Ferma") : tr("Converti"));
    m_runButton->setEnabled(true);
}

// ---------------------------------------------------------------------------
// The run

void ConvertPage::run()
{
    if (m_process || m_probing > 0)
        return;
    const ConvertOptions o = options();
    QList<ConvertItem *> todo;
    QList<ConvertItem *> done;
    for (ConvertItem *item : std::as_const(m_items)) {
        if (item->state == ConvertItem::Probing || item->state == ConvertItem::Unreadable
            || !plan(item->info, o).error.isEmpty())
            continue;
        (item->state == ConvertItem::Done ? done : todo) << item;
    }
    // Files added later, stopped or failed come first; with none of them the
    // button converts the same files once more.
    if (todo.isEmpty())
        todo = done;
    if (todo.isEmpty())
        return;
    const QString dir = directory();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        setStatus(tr("La cartella \"%1\" non esiste").arg(dir), true);
        return;
    }

    m_runOptions = o;
    m_runDirectory = dir;
    m_queue = todo;
    m_runTotal = int(todo.size());
    m_runIndex = 0;
    m_runDone = 0;
    m_runFailed = 0;
    m_runBytes = 0;
    m_lastError.clear();
    m_stopping = false;
    m_weighByDuration = std::all_of(todo.cbegin(), todo.cend(),
                                    [](const ConvertItem *item) { return item->info.duration > 0; });
    m_weightDone = 0;
    m_weightTotal = 0;
    for (ConvertItem *item : std::as_const(todo)) {
        m_weightTotal += weight(item);
        item->state = ConvertItem::Queued;
        item->error.clear();
        item->output.clear();
        updateRow(item, {});
    }
    m_doneFile.clear();
    m_revealButton->hide();
    setRunning(true);
    m_runStarted.start();
    startNext();
}

double ConvertPage::weight(const ConvertItem *item) const
{
    return m_weighByDuration ? item->info.duration : 1.0;
}

// The next file of the queue, or the end of the run.
void ConvertPage::startNext()
{
    while (!m_queue.isEmpty()) {
        m_current = m_queue.takeFirst();
        ++m_runIndex;
        m_maxKbps = 0;
        m_attempt = 1;
        QString error;
        if (!QFileInfo(m_current->info.path).isFile()) {
            error = tr("Il file non c'è più");
        } else {
            const ConvertPlan p = plan(m_current->info, m_runOptions);
            error = p.error;
            if (error.isEmpty()) {
                // Named only now: two files that give the same name do not collide.
                m_output = outputPath(m_current->info.path, m_runDirectory, p);
                m_current->state = ConvertItem::Running;
                m_drop->showRow(m_current->row);
                error = launch();
            }
        }
        if (error.isEmpty())
            return; // finished() goes on from here
        failCurrent(error);
    }
    finishRun();
}

// Starts ffmpeg on the current file; returns why it could not.
QString ConvertPage::launch()
{
    const QString program = findTool(QStringLiteral("ffmpeg"));
    if (program.isEmpty())
        return tr("ffmpeg non trovato");
    m_runPlan = plan(m_current->info, m_runOptions, m_maxKbps);
    if (!m_runPlan.error.isEmpty())
        return m_runPlan.error;

    QStringList args = {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                        QStringLiteral("-nostdin"), QStringLiteral("-n"),
                        // Read back by readProgress(): how far the output has got.
                        QStringLiteral("-progress"), QStringLiteral("pipe:1"), QStringLiteral("-nostats"),
                        QStringLiteral("-i"), m_current->info.path};
    args << m_runPlan.arguments << m_output;

    m_progressBuffer.clear();
    m_errors.clear();
    QProcess *process = new QProcess(this);
    m_process = process;
    process->setProcessEnvironment(toolEnvironment());
    connect(process, &QProcess::readyReadStandardOutput, this, &ConvertPage::readProgress);
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process] { m_errors += process->readAllStandardError(); });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        if (m_process == process)
            finished(true, status == QProcess::NormalExit ? code : -1);
    });
    // Queued: a program that cannot start says so from inside start().
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (m_process == process && error == QProcess::FailedToStart)
            finished(false, -1);
    }, Qt::QueuedConnection);

    const QString starting = m_attempt > 1 ? tr("Era ancora troppo grande: nuovo tentativo…") : tr("Avvio…");
    m_current->row->setProgress(-1, m_attempt > 1 ? tr("Nuovo tentativo…") : tr("Avvio…"));
    if (m_runTotal > 1) {
        // The bar below is the whole list; the row has its own.
        m_bar->setRange(0, 1000);
        m_bar->setValue(int(m_weightDone / m_weightTotal * 1000));
        setStatus(tr("File %1 di %2").arg(m_runIndex).arg(m_runTotal));
    } else {
        m_bar->setRange(0, 0);
        setStatus(starting);
    }
    m_started.start();
    process->start(program, args);
    process->closeWriteChannel();
    return {};
}

void ConvertPage::stop()
{
    if (!m_process || m_stopping)
        return;
    m_stopping = true;
    setStatus(tr("Arresto…"));
    stopTool(m_process);
}

// ffmpeg writes "key=value" lines; out_time_us is the position reached in the output.
void ConvertPage::readProgress()
{
    if (!m_process || !m_current)
        return;
    m_progressBuffer += m_process->readAllStandardOutput();
    qint64 position = -1;
    qsizetype end;
    while ((end = m_progressBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_progressBuffer.left(end).trimmed();
        m_progressBuffer.remove(0, end + 1);
        if (line.startsWith("out_time_us="))
            position = line.mid(12).toLongLong();
    }
    if (position < 0 || m_stopping || m_current->info.duration <= 0)
        return;
    const double fraction = qBound(0.0, position / 1e6 / m_current->info.duration, 1.0);
    const auto percent = [](double value) { return QStringLiteral("%1%").arg(int(value * 100)); };
    m_current->row->setProgress(int(fraction * 1000), percent(fraction));

    // Below: the file when it is alone, otherwise the whole list.
    const bool list = m_runTotal > 1;
    const double whole = list ? qBound(0.0, (m_weightDone + weight(m_current) * fraction) / m_weightTotal, 1.0)
                              : fraction;
    const qint64 elapsed = list ? m_runStarted.elapsed() : m_started.elapsed();
    m_bar->setRange(0, 1000);
    m_bar->setValue(int(whole * 1000));
    QString text = list ? tr("File %1 di %2").arg(m_runIndex).arg(m_runTotal) + QStringLiteral("  ·  ") + percent(whole)
                        : percent(whole);
    if (whole > 0.02 && whole < 1 && elapsed > 1500) {
        const int left = qMax(1, int(elapsed / 1000.0 * (1 - whole) / whole));
        text += QStringLiteral("  ·  ") + tr("ancora %1").arg(formatDuration(left));
    }
    setStatus(text);
}

void ConvertPage::finished(bool started, int exitCode)
{
    if (!m_process || !m_current)
        return;
    if (started)
        m_errors += m_process->readAllStandardError();
    m_process->disconnect();
    m_process->deleteLater();
    m_process = nullptr;

    const bool ok = started && exitCode == 0 && !m_stopping && QFileInfo::exists(m_output);
    if (ok && m_runOptions.limitMb > 0) {
        // The size was estimated: when the file still came out too big it is redone lower.
        const qint64 limit = qint64(m_runOptions.limitMb) * 1000 * 1000;
        const qint64 size = QFileInfo(m_output).size();
        if (size > limit && m_attempt < 4 && m_runPlan.kbps > 1) {
            QFile::remove(m_output);
            m_maxKbps = qMin(m_runPlan.kbps - 1, int(m_runPlan.kbps * double(limit) / size * 0.96));
            ++m_attempt;
            const QString error = launch();
            if (!error.isEmpty()) {
                failCurrent(error);
                startNext();
            }
            return;
        }
    }

    if (ok) {
        ConvertItem *item = m_current;
        m_current = nullptr;
        item->state = ConvertItem::Done;
        item->output = m_output;
        m_doneFile = m_output;
        ++m_runDone;
        m_runBytes += QFileInfo(m_output).size();
        m_weightDone += weight(item);
        updateRow(item, {});
        startNext();
        return;
    }

    QFile::remove(m_output); // half a file is of no use
    if (!m_stopping) {
        const QStringList lines = QString::fromUtf8(m_errors).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        failCurrent(!started ? tr("Impossibile avviare ffmpeg")
                    : lines.isEmpty() ? tr("Conversione non riuscita") : lines.last().trimmed());
        startNext();
        return;
    }
    // Stopped: the files still waiting go back to how they were.
    m_current->state = ConvertItem::Stopped;
    updateRow(m_current, {});
    m_current = nullptr;
    for (ConvertItem *item : std::as_const(m_queue)) {
        item->state = ConvertItem::Waiting;
        updateRow(item, {});
    }
    m_queue.clear();
    finishRun();
}

// The current file could not be converted: the run goes on with the others.
void ConvertPage::failCurrent(const QString &error)
{
    ConvertItem *item = m_current;
    m_current = nullptr;
    item->state = ConvertItem::Failed;
    item->error = error;
    m_lastError = error;
    ++m_runFailed;
    m_weightDone += weight(item);
    updateRow(item, {});
}

void ConvertPage::finishRun()
{
    const bool stopped = m_stopping;
    m_stopping = false;
    setRunning(false);
    m_bar->setRange(0, 1000);
    m_bar->setValue(m_runDone == m_runTotal ? 1000 : 0);
    m_revealButton->setVisible(m_runDone > 0);
    if (m_runTotal == 1) {
        if (m_runDone > 0)
            setStatus(tr("Fatto  ·  %1").arg(sizeText(m_runBytes)));
        else if (stopped)
            setStatus(tr("Interrotto"));
        else
            setStatus(m_lastError, true);
    } else if (stopped) {
        setStatus(tr("Interrotto  ·  completati %1 di %2").arg(m_runDone).arg(m_runTotal));
    } else if (m_runFailed == 0) {
        setStatus(tr("Fatto  ·  %1  ·  %2").arg(tr("%n file", nullptr, m_runDone), sizeText(m_runBytes)));
    } else {
        setStatus(tr("Completati %1 di %2  ·  non riusciti: %3").arg(m_runDone).arg(m_runTotal).arg(m_runFailed),
                  true);
    }
    refresh();
    // Until a choice changes, the line names what was just written instead of the next result.
    if (m_runDone > 0 && m_runTotal == 1) {
        m_resultLabel->setFullText(tr("Creato:  %1").arg(QFileInfo(m_doneFile).fileName()));
    } else if (m_runDone > 0) {
        m_resultLabel->setFullText(
            tr("Creati:  %1  ·  %2")
                .arg(tr("%n file", nullptr, m_runDone),
                     m_runDirectory.isEmpty() ? tr("accanto ai file di partenza")
                                              : QDir::toNativeSeparators(QFileInfo(m_doneFile).absolutePath())));
    }
}

// ---------------------------------------------------------------------------
// Drag and drop

static QStringList droppedFiles(const QMimeData *data)
{
    QStringList files;
    if (!data->hasUrls())
        return files;
    const QList<QUrl> urls = data->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile())
            files << url.toLocalFile();
    }
    return files;
}

void ConvertPage::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_process || droppedFiles(event->mimeData()).isEmpty())
        return;
    event->acceptProposedAction();
    m_drop->setHighlighted(true);
}

void ConvertPage::dragLeaveEvent(QDragLeaveEvent *)
{
    m_drop->setHighlighted(false);
}

// What is dropped joins the list.
void ConvertPage::dropEvent(QDropEvent *event)
{
    m_drop->setHighlighted(false);
    const QStringList files = droppedFiles(event->mimeData());
    if (m_process || files.isEmpty())
        return;
    event->acceptProposedAction();
    addInputs(files);
}
