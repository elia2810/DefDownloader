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
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

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
// FileDrop

FileDrop::FileDrop(QWidget *parent)
    : QFrame(parent)
{
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(150);

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

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(8);
    layout->addStretch(1);
    layout->addWidget(icon, 0, Qt::AlignHCenter);
    layout->addWidget(m_title);
    layout->addWidget(m_details);
    layout->addSpacing(4);
    layout->addWidget(m_button, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    setFile({}, {});
}

void FileDrop::setFile(const QString &name, const QString &details, bool error)
{
    const bool empty = name.isEmpty();
    m_title->setFullText(empty ? tr("Trascina qui un file video o audio") : name);
    m_details->setFullText(empty ? tr("oppure sceglilo dal computer") : details);
    m_button->setText(empty ? tr("Scegli file…") : tr("Cambia file…"));

    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, error ? kErrorColor : pal.color(QPalette::PlaceholderText));
    m_details->setPalette(pal);
}

void FileDrop::setHighlighted(bool on)
{
    if (m_highlighted == on)
        return;
    m_highlighted = on;
    update();
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

void FileDrop::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
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
    connect(m_drop, &FileDrop::clicked, this, &ConvertPage::browse);

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
    m_dirEdit->setPlaceholderText(tr("Nella stessa cartella del file"));
    m_dirEdit->setClearButtonEnabled(true);
    m_dirButton = new QPushButton(tr("Sfoglia…"));
    m_dirButton->setAutoDefault(false);
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(m_dirEdit, 1);
    dirRow->addWidget(m_dirButton);
    grid->addWidget(rowLabel(tr("Salva in")), 3, 0);
    grid->addLayout(dirRow, 3, 1);
    connect(m_dirButton, &QPushButton::clicked, this, [this] {
        const QString start = directory().isEmpty() ? QFileInfo(m_info.path).absolutePath() : directory();
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Cartella di destinazione"), start);
        if (!dir.isEmpty()) {
            m_dirEdit->setText(dir);
            refresh();
        }
    });
    connect(m_dirEdit, &QLineEdit::textChanged, this, &ConvertPage::refresh);

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
        refresh();
        if (m_otherFormat->isVisible())
            m_otherFormat->setFocus();
    });
    connect(m_otherFormat, &QLineEdit::textChanged, this, &ConvertPage::refresh);
    for (QCheckBox *check : {m_limit64, m_limit16, m_denoise, m_voice, m_normalize, m_trimSilence, m_mono})
        connect(check, &QCheckBox::toggled, this, &ConvertPage::refresh);

    connect(qApp, &QCoreApplication::aboutToQuit, this, &ConvertPage::shutDown);
    refresh();
}

ConvertPage::~ConvertPage()
{
    shutDown();
}

// A conversion cut short leaves half a file: it is stopped and removed.
void ConvertPage::shutDown()
{
    if (m_probe) {
        m_probe->disconnect();
        m_probe->kill();
        m_probe->waitForFinished(500);
        m_probe = nullptr;
    }
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

void ConvertPage::browse()
{
    if (m_process)
        return;
    QSettings settings;
    const QString start = m_info.path.isEmpty() ? settings.value(QStringLiteral("convert/lastDir"), downloadsDir()).toString()
                                                : QFileInfo(m_info.path).absolutePath();
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Scegli un file video o audio"), start,
        tr("Video e audio (*.mp4 *.mkv *.mov *.webm *.avi *.m4v *.wmv *.flv *.ts *.mpg *.mpeg *.3gp *.gif "
           "*.mp3 *.m4a *.aac *.opus *.ogg *.oga *.flac *.wav *.wma *.aiff *.aif *.amr *.m4b *.mka);;Tutti i file (*)"));
    if (!file.isEmpty())
        setInput(file);
}

void ConvertPage::setInput(const QString &path)
{
    if (m_process)
        return;
    if (m_probe) {
        m_probe->disconnect();
        m_probe->kill();
        m_probe->deleteLater();
        m_probe = nullptr;
    }
    m_info = MediaInfo();
    m_info.path = path;
    m_inputError.clear();
    m_doneFile.clear();
    m_revealButton->hide();
    m_bar->setRange(0, 1000);
    m_bar->setValue(0);
    setStatus({});

    const QFileInfo file(path);
    const auto fail = [this, file](const QString &message) {
        m_inputError = message;
        m_drop->setFile(file.fileName(), message, true);
        refresh();
    };
    if (!file.isFile()) {
        fail(tr("Non è un file"));
        return;
    }
    QSettings().setValue(QStringLiteral("convert/lastDir"), file.absolutePath());
    const QString program = findTool(QStringLiteral("ffprobe"));
    if (program.isEmpty()) {
        fail(tr("ffprobe non trovato"));
        return;
    }

    m_drop->setFile(file.fileName(), tr("Lettura del file…"));
    m_probe = new QProcess(this);
    QProcess *probe = m_probe;
    probe->setProcessEnvironment(toolEnvironment());
    connect(probe, &QProcess::finished, this, [this, probe, path, file, fail] {
        const MediaInfo info = parseProbe(probe->readAllStandardOutput(), path);
        probe->deleteLater();
        m_probe = nullptr;
        if (!info.valid) {
            fail(tr("Non è un file video o audio che ffmpeg sa leggere"));
            return;
        }
        m_info = info;
        m_drop->setFile(file.fileName(), describe(info));
        refresh();
    });
    connect(probe, &QProcess::errorOccurred, this, [this, probe, fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            probe->deleteLater();
            m_probe = nullptr;
            fail(tr("Impossibile avviare ffprobe"));
        }
    });
    probe->start(program, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                           QStringLiteral("format=duration,size,bit_rate:stream=codec_type,codec_name,width,height,"
                                          "bit_rate,sample_rate,channels:stream_disposition=attached_pic"),
                           QStringLiteral("-of"), QStringLiteral("json"), path});
    probe->closeWriteChannel();
    refresh();
}

// Shows what the current choices would produce.
void ConvertPage::refresh()
{
    saveOptions();
    m_otherFormat->setVisible(m_format->currentData().toString() == kOtherFormat);
    if (m_process)
        return;

    const ConvertPlan p = plan(m_info, options());
    // Removing pauses only makes sense when no video has to stay in sync.
    m_trimSilence->setEnabled(!m_info.valid || p.audioOnly);

    QPalette pal = palette();
    QString text;
    if (!m_inputError.isEmpty() || m_probe) {
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
    } else if (!p.error.isEmpty()) {
        pal.setColor(QPalette::WindowText, p.nothingToDo ? pal.color(QPalette::PlaceholderText) : kErrorColor);
        text = p.error;
    } else {
        QStringList parts = {QFileInfo(outputPath(m_info.path, directory(), p)).fileName(), p.details};
        if (p.estimatedBytes > 0) {
            parts << (options().trimSilence ? tr("al massimo %1") : tr("circa %1")).arg(sizeText(p.estimatedBytes));
        }
        text = tr("Risultato:  %1").arg(parts.join(QStringLiteral("  ·  ")));
    }
    m_resultLabel->setPalette(pal);
    m_resultLabel->setFullText(text);
    m_runButton->setEnabled(m_info.valid && p.error.isEmpty());
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
    for (QWidget *widget : std::initializer_list<QWidget *>{m_drop, m_format, m_otherFormat, m_limit64, m_limit16,
                                                            m_denoise, m_voice, m_normalize, m_trimSilence, m_mono,
                                                            m_dirEdit, m_dirButton})
        widget->setEnabled(!running);
    m_runButton->setText(running ? tr("Ferma") : tr("Converti"));
    m_runButton->setEnabled(true);
}

void ConvertPage::run()
{
    if (m_process || !m_info.valid)
        return;
    if (!QFileInfo(m_info.path).isFile()) {
        setStatus(tr("Il file non c'è più"), true);
        return;
    }
    m_runOptions = options();
    m_maxKbps = 0;
    m_attempt = 1;
    const ConvertPlan p = plan(m_info, m_runOptions);
    if (!p.error.isEmpty())
        return;
    const QString dir = directory();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        setStatus(tr("La cartella \"%1\" non esiste").arg(dir), true);
        return;
    }
    m_output = outputPath(m_info.path, dir, p);
    m_doneFile.clear();
    m_revealButton->hide();
    launch();
}

void ConvertPage::launch()
{
    const QString program = findTool(QStringLiteral("ffmpeg"));
    if (program.isEmpty()) {
        setRunning(false);
        setStatus(tr("ffmpeg non trovato"), true);
        return;
    }
    m_runPlan = plan(m_info, m_runOptions, m_maxKbps);
    if (!m_runPlan.error.isEmpty()) {
        setRunning(false);
        setStatus(m_runPlan.error, true);
        refresh();
        return;
    }

    QStringList args = {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                        QStringLiteral("-nostdin"), QStringLiteral("-n"),
                        // Read back by readProgress(): how far the output has got.
                        QStringLiteral("-progress"), QStringLiteral("pipe:1"), QStringLiteral("-nostats"),
                        QStringLiteral("-i"), m_info.path};
    args << m_runPlan.arguments << m_output;

    m_progressBuffer.clear();
    m_errors.clear();
    m_stopping = false;
    m_process = new QProcess(this);
    m_process->setProcessEnvironment(toolEnvironment());
    connect(m_process, &QProcess::readyReadStandardOutput, this, &ConvertPage::readProgress);
    connect(m_process, &QProcess::readyReadStandardError, this,
            [this] { m_errors += m_process->readAllStandardError(); });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finished(true, status == QProcess::NormalExit ? code : -1);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finished(false, -1);
    });

    setRunning(true);
    m_bar->setRange(0, 0);
    setStatus(m_attempt > 1 ? tr("Era ancora troppo grande: nuovo tentativo…") : tr("Avvio…"));
    m_started.start();
    m_process->start(program, args);
    m_process->closeWriteChannel();
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
    m_progressBuffer += m_process->readAllStandardOutput();
    qint64 position = -1;
    qsizetype end;
    while ((end = m_progressBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_progressBuffer.left(end).trimmed();
        m_progressBuffer.remove(0, end + 1);
        if (line.startsWith("out_time_us="))
            position = line.mid(12).toLongLong();
    }
    if (position < 0 || m_stopping || m_info.duration <= 0)
        return;
    const double fraction = qBound(0.0, position / 1e6 / m_info.duration, 1.0);
    m_bar->setRange(0, 1000);
    m_bar->setValue(int(fraction * 1000));
    QString text = QStringLiteral("%1%").arg(int(fraction * 100));
    if (fraction > 0.02 && fraction < 1 && m_started.elapsed() > 1500) {
        const int left = qMax(1, int(m_started.elapsed() / 1000.0 * (1 - fraction) / fraction));
        text += QStringLiteral("  ·  ") + tr("ancora %1").arg(formatDuration(left));
    }
    setStatus(text);
}

void ConvertPage::finished(bool started, int exitCode)
{
    if (!m_process)
        return;
    if (started)
        m_errors += m_process->readAllStandardError();
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
            launch();
            return;
        }
    }

    setRunning(false);
    m_bar->setRange(0, 1000);
    if (ok) {
        m_doneFile = m_output;
        m_bar->setValue(1000);
        m_revealButton->show();
        setStatus(tr("Fatto  ·  %1").arg(sizeText(QFileInfo(m_output).size())));
    } else {
        QFile::remove(m_output); // half a file is of no use
        m_bar->setValue(0);
        if (m_stopping) {
            setStatus(tr("Interrotto"));
        } else if (!started) {
            setStatus(tr("Impossibile avviare ffmpeg"), true);
        } else {
            const QStringList lines = QString::fromUtf8(m_errors).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            setStatus(lines.isEmpty() ? tr("Conversione non riuscita") : lines.last().trimmed(), true);
        }
    }
    m_stopping = false;
    refresh();
    // Until a choice changes, the line names the file just written instead of the next one.
    if (ok)
        m_resultLabel->setFullText(tr("Creato:  %1").arg(QFileInfo(m_doneFile).fileName()));
}

// ---------------------------------------------------------------------------
// Drag and drop

static QString droppedFile(const QMimeData *data)
{
    if (!data->hasUrls())
        return {};
    for (const QUrl &url : data->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile())
            return url.toLocalFile();
    }
    return {};
}

void ConvertPage::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_process || droppedFile(event->mimeData()).isEmpty())
        return;
    event->acceptProposedAction();
    m_drop->setHighlighted(true);
}

void ConvertPage::dragLeaveEvent(QDragLeaveEvent *)
{
    m_drop->setHighlighted(false);
}

void ConvertPage::dropEvent(QDropEvent *event)
{
    m_drop->setHighlighted(false);
    const QString file = droppedFile(event->mimeData());
    if (m_process || file.isEmpty())
        return;
    event->acceptProposedAction();
    setInput(file);
}
