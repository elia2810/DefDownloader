#include "simple.h"

#include "tools.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace {

const QList<int> kHeights = {144, 240, 360, 480, 720, 1080, 1440, 2160, 0}; // 0: the best available
const QList<int> kBitrates = {64, 96, 128, 160, 192, 256, 320};
const QStringList kVideoFormats = {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm")};
const QStringList kAudioFormats = {QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("opus"),
                                   QStringLiteral("flac"), QStringLiteral("wav")};
const int kMaxParallel = 3;
const QColor kErrorColor(0xE0, 0x4F, 0x45);

bool isLossless(const QString &format)
{
    return format == QLatin1String("flac") || format == QLatin1String("wav");
}

QString formatDuration(int seconds)
{
    if (seconds <= 0)
        return {};
    const int h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString formatSpeed(double bytesPerSecond)
{
    const QLocale locale;
    if (bytesPerSecond >= 1024 * 1024)
        return locale.toString(bytesPerSecond / (1024 * 1024), 'f', 1) + QStringLiteral(" MB/s");
    return locale.toString(bytesPerSecond / 1024, 'f', 0) + QStringLiteral(" kB/s");
}

// "ERROR: [youtube] abc123: Video unavailable" -> "Video unavailable"
QString cleanError(const QString &output)
{
    static const QRegularExpression prefix(QStringLiteral("^ERROR:\\s*(?:\\[[^\\]]+\\]\\s+\\S+:\\s+)?"));
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        if (it->startsWith(QLatin1String("ERROR:")))
            return QString(*it).remove(prefix).trimmed();
    }
    return {};
}

bool isUrl(const QString &token)
{
    static const QRegularExpression bare(QStringLiteral("^([\\w-]+\\.)+[a-z]{2,}/\\S*$"),
                                         QRegularExpression::CaseInsensitiveOption);
    return token.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
           || token.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) || bare.match(token).hasMatch();
}

// "watch?v=...&list=..." names a video and the playlist it was opened from:
// the playlist is what gets added. YouTube's automatic mixes ("RD...") are
// endless and personal lists (watch later, liked) need a login, so for those
// only the video is taken.
bool wantsWholePlaylist(const QString &url)
{
    const QString list = QUrlQuery(QUrl(url)).queryItemValue(QStringLiteral("list"));
    return !(list.startsWith(QLatin1String("RD")) || list == QLatin1String("WL") || list == QLatin1String("LL"));
}

QStringList splitTokens(const QString &text)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    return text.split(spaces, Qt::SkipEmptyParts);
}

bool allUrls(const QStringList &tokens)
{
    return !tokens.isEmpty() && std::all_of(tokens.cbegin(), tokens.cend(), isUrl);
}

QLabel *smallLabel(const QString &text = {})
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    return label;
}

} // namespace

// ---------------------------------------------------------------------------
// DownloadRow

DownloadRow::DownloadRow(Service service, Engine engine, const QString &target, const DownloadSettings &settings,
                         QWidget *parent)
    : QFrame(parent)
    , m_service(service)
    , m_engine(engine)
    , m_target(target)
    , m_settings(settings)
    , m_title(target)
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 9, 8, 10);
    root->setSpacing(10);

    auto *icon = new QLabel;
    icon->setPixmap(servicePixmap(service, 26, qApp->devicePixelRatio()));
    icon->setFixedSize(26, 26);
    root->addWidget(icon, 0, Qt::AlignVCenter);

    auto *middle = new QVBoxLayout;
    middle->setSpacing(3);
    m_titleLabel = new ElidedLabel;
    QFont bold = m_titleLabel->font();
    bold.setBold(true);
    m_titleLabel->setFont(bold);
    m_subtitleLabel = new ElidedLabel;
    QFont small = m_subtitleLabel->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    m_subtitleLabel->setFont(small);

    auto *progress = new QHBoxLayout;
    progress->setSpacing(8);
    m_bar = new QProgressBar;
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(8);
    m_statusLabel = smallLabel();
    m_statusLabel->setMinimumWidth(170);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progress->addWidget(m_bar, 1);
    progress->addWidget(m_statusLabel);

    middle->addWidget(m_titleLabel);
    middle->addWidget(m_subtitleLabel);
    middle->addLayout(progress);
    root->addLayout(middle, 1);

    m_modeCombo = new QComboBox;
    m_modeCombo->addItems({tr("Video + audio"), tr("Solo video"), tr("Solo audio")});
    m_modeCombo->setCurrentIndex(settings.mode);
    m_modeCombo->setToolTip(tr("Cosa scaricare per questo elemento"));
    root->addWidget(m_modeCombo, 0, Qt::AlignVCenter);
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_settings.mode = DownloadSettings::Mode(index);
        updateSubtitle();
    });

    const auto button = [this, root] {
        auto *b = new QToolButton;
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(28, 28);
        b->setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 6px; }"
                                        "QToolButton:hover { background: rgba(128, 128, 128, 0.25); }"
                                        "QToolButton:pressed { background: rgba(128, 128, 128, 0.4); }"));
        b->setCursor(Qt::PointingHandCursor);
        root->addWidget(b, 0, Qt::AlignVCenter);
        return b;
    };
    m_actionButton = button();
    m_removeButton = button();
    m_removeButton->setToolTip(tr("Elimina dalla lista"));
    connect(m_removeButton, &QToolButton::clicked, this, &DownloadRow::removeRequested);
    connect(m_actionButton, &QToolButton::clicked, this, [this] {
        switch (m_state) {
        case Ready:
        case Failed:
            emit downloadRequested();
            break;
        case Queued:
            setState(Ready);
            break;
        case Running:
            stop();
            break;
        case Done:
            reveal();
            break;
        case Resolving:
            break;
        }
    });

    setState(Resolving);
}

DownloadRow::~DownloadRow()
{
    // Covers the download and the process that reads the link.
    const QList<QProcess *> processes = findChildren<QProcess *>();
    for (QProcess *process : processes)
        abandonTool(process);
}

void DownloadRow::setTarget(Engine engine, const QString &target)
{
    m_engine = engine;
    m_target = target;
}

void DownloadRow::setInfo(const QString &title, const QString &author, int seconds)
{
    m_title = title;
    m_author = author;
    m_seconds = seconds;
    setState(Ready);
}

void DownloadRow::setFailed(const QString &message)
{
    m_error = message;
    setState(Failed);
}

void DownloadRow::setQueued()
{
    if (m_state == Ready || m_state == Failed)
        setState(Queued);
}

void DownloadRow::setState(State state, const QString &status)
{
    m_state = state;
    QString text = status;
    m_bar->setRange(0, 1000);
    switch (state) {
    case Resolving:
        m_bar->setRange(0, 0);
        text = tr("Lettura del link…");
        break;
    case Ready:
        m_bar->setValue(0);
        break;
    case Queued:
        m_bar->setValue(0);
        text = tr("In coda");
        break;
    case Running:
        m_bar->setRange(0, 0);
        text = tr("Avvio…");
        break;
    case Done:
        m_bar->setValue(1000);
        text = tr("Completato");
        break;
    case Failed:
        m_bar->setValue(0);
        text = tr("Non riuscito");
        break;
    }
    m_statusLabel->setText(text);
    refresh();
    emit stateChanged();
}

void DownloadRow::refresh()
{
    const QColor color = palette().color(QPalette::WindowText);
    Glyph glyph = Glyph::Download;
    QString tip = tr("Scarica");
    switch (m_state) {
    case Resolving:
    case Ready:
        break;
    case Queued:
        glyph = Glyph::Stop;
        tip = tr("Togli dalla coda");
        break;
    case Running:
        glyph = Glyph::Stop;
        tip = tr("Ferma");
        break;
    case Done:
        glyph = Glyph::Folder;
        tip = tr("Mostra nella cartella");
        break;
    case Failed:
        glyph = Glyph::Retry;
        tip = tr("Riprova");
        break;
    }
    m_actionButton->setIcon(glyphIcon(glyph, color));
    m_actionButton->setToolTip(tip);
    m_actionButton->setEnabled(m_state != Resolving);
    m_removeButton->setIcon(glyphIcon(Glyph::Trash, color));

    // Only yt-dlp can split video and audio.
    m_modeCombo->setVisible(m_engine == YtDlp);
    m_modeCombo->setEnabled(m_state == Resolving || m_state == Ready || m_state == Failed);

    m_titleLabel->setFullText(m_title);
    updateSubtitle();
}

void DownloadRow::updateSubtitle()
{
    QPalette pal = palette();
    if (m_state == Failed && !m_error.isEmpty()) {
        pal.setColor(QPalette::WindowText, kErrorColor);
        m_subtitleLabel->setPalette(pal);
        m_subtitleLabel->setFullText(m_error);
        return;
    }
    pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
    m_subtitleLabel->setPalette(pal);

    QStringList parts;
    if (!m_author.isEmpty())
        parts << m_author;
    if (m_seconds > 0)
        parts << formatDuration(m_seconds);
    if (m_engine != Instaloader) {
        const QString video = m_settings.videoFormat + QLatin1Char(' ')
                              + (m_settings.height > 0 ? QStringLiteral("%1p").arg(m_settings.height)
                                                       : tr("qualità massima"));
        const QString audio = isLossless(m_settings.audioFormat)
                                  ? m_settings.audioFormat
                                  : QStringLiteral("%1 %2 kbps").arg(m_settings.audioFormat).arg(m_settings.bitrate);
        if (m_engine == SpotDl || m_settings.mode == DownloadSettings::AudioOnly)
            parts << audio;
        else if (m_settings.mode == DownloadSettings::VideoOnly)
            parts << video + tr(", senza audio");
        else
            parts << video;
    }
    m_subtitleLabel->setFullText(parts.join(QStringLiteral("  ·  ")));
}

void DownloadRow::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_actionButton)
        refresh(); // the icons follow the text colour
}

void DownloadRow::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);
    QPainter p(this);
    QColor line = palette().color(QPalette::WindowText);
    line.setAlphaF(0.12);
    p.setPen(line);
    p.drawLine(12, height() - 1, width() - 12, height() - 1);
}

QStringList DownloadRow::arguments(const QString &directory) const
{
    const DownloadSettings &s = m_settings;
    const QString bitrate = QString::number(s.bitrate);

    if (m_engine == SpotDl) {
        QStringList args = {QStringLiteral("download"), m_target, QStringLiteral("--format"), s.audioFormat};
        if (!isLossless(s.audioFormat))
            args << QStringLiteral("--bitrate") << bitrate + QLatin1Char('k');
        return args;
    }
    if (m_engine == Instaloader) {
        QStringList args = {QStringLiteral("--no-metadata-json"), QStringLiteral("--no-captions"),
                            QStringLiteral("--dirname-pattern"), QStringLiteral("{profile}")};
        if (m_target.startsWith(QLatin1Char('-')))
            args << QStringLiteral("--"); // a post: "-SHORTCODE"
        return args << m_target;
    }

    QStringList args = {
        QStringLiteral("--no-playlist"), QStringLiteral("--newline"), QStringLiteral("--no-colors"),
        QStringLiteral("--no-quiet"), QStringLiteral("-P"), directory,
        // Read back by handleYtDlpLine(): total size, progress, final file.
        QStringLiteral("--print"), QStringLiteral("before_dl:DLT|%(filesize,filesize_approx)s"),
        QStringLiteral("--print"), QStringLiteral("after_move:DLF|%(filepath)s"),
        QStringLiteral("--progress-template"),
        QStringLiteral("download:DLP|%(progress.downloaded_bytes)s|%(progress.total_bytes)s"
                       "|%(progress.total_bytes_estimate)s|%(progress.speed)s|%(progress.eta)s"),
    };

    if (s.mode == DownloadSettings::AudioOnly) {
        args << QStringLiteral("-f") << QStringLiteral("ba/b") << QStringLiteral("-x")
             << QStringLiteral("--audio-format") << s.audioFormat;
        if (!isLossless(s.audioFormat))
            args << QStringLiteral("--audio-quality") << bitrate + QLatin1Char('K');
        return args << m_target;
    }

    // The order is the priority: resolution first, then codecs that the
    // chosen container plays everywhere, then the audio bitrate.
    QStringList sort = {s.height > 0 ? QStringLiteral("res:%1").arg(s.height) : QStringLiteral("res"),
                        QStringLiteral("fps")};
    if (s.videoFormat == QLatin1String("mp4"))
        sort << QStringLiteral("vcodec:h264") << QStringLiteral("acodec:aac");
    else if (s.videoFormat == QLatin1String("webm"))
        sort << QStringLiteral("vcodec:vp9") << QStringLiteral("acodec:opus");
    if (s.mode == DownloadSettings::Both)
        sort << QStringLiteral("abr:") + bitrate;
    args << QStringLiteral("-S") << sort.join(QLatin1Char(','));
    // "bv" has no audio; sites that only offer joined files fall back to "b".
    args << QStringLiteral("-f")
         << (s.mode == DownloadSettings::Both ? QStringLiteral("bv*+ba/b") : QStringLiteral("bv/b"));
    if (s.videoFormat == QLatin1String("webm")) {
        // webm only holds vp8/vp9/av1: anything else is converted.
        args << QStringLiteral("--recode-video") << s.videoFormat;
    } else {
        args << QStringLiteral("--merge-output-format") << s.videoFormat << QStringLiteral("--remux-video")
             << s.videoFormat;
    }
    return args << m_target;
}

void DownloadRow::start(const QString &directory)
{
    if (m_process)
        return;
    const QString tool = m_engine == SpotDl ? QStringLiteral("spotdl")
                         : m_engine == Instaloader ? QStringLiteral("instaloader") : QStringLiteral("yt-dlp");
    const QString program = findTool(tool);
    if (program.isEmpty()) {
        setFailed(tr("%1 non trovato").arg(tool));
        return;
    }
    if (!QDir().mkpath(directory)) {
        setFailed(tr("La cartella \"%1\" non esiste").arg(directory));
        return;
    }

    m_directory = directory;
    m_filePath.clear();
    m_error.clear();
    m_lastLine.clear();
    m_buffer.clear();
    m_stopping = m_succeeded = false;
    m_expectedBytes = m_doneBytes = m_fileBytes = m_fileTotal = 0;
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    m_lastUpdate.invalidate();

    m_process = new QProcess(this);
    m_process->setProcessEnvironment(toolEnvironment());
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setWorkingDirectory(directory);
    connect(m_process, &QProcess::readyRead, this, [this] { consume(m_decoder.decode(m_process->readAll())); });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finished(true, status == QProcess::NormalExit ? code : -1);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this, tool](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_error = tr("Impossibile avviare %1").arg(tool);
            finished(false, -1);
        }
    });

    setState(Running);
    m_process->start(program, arguments(directory));
    m_process->closeWriteChannel(); // nothing may wait for an answer
}

void DownloadRow::stop()
{
    if (!m_process)
        return;
    m_stopping = true;
    m_statusLabel->setText(tr("Arresto…"));
    stopTool(m_process);
}

void DownloadRow::finished(bool started, int exitCode)
{
    if (!m_process)
        return;
    if (started) {
        consume(m_decoder.decode(m_process->readAll()));
        if (!m_buffer.isEmpty())
            handleLine(m_buffer);
    }
    m_buffer.clear();
    m_process->deleteLater();
    m_process = nullptr;

    if (m_stopping) {
        m_stopping = false;
        setState(Ready, tr("Interrotto"));
        return;
    }
    // spotdl also ends with code 0 when it finds nothing to download.
    if (started && exitCode == 0 && (m_engine != SpotDl || m_succeeded)) {
        setState(Done);
        return;
    }
    if (m_error.isEmpty())
        m_error = m_lastLine.isEmpty() ? tr("Download non riuscito") : m_lastLine;
    setState(Failed);
}

void DownloadRow::setBusy(const QString &status)
{
    m_bar->setRange(0, 0);
    m_statusLabel->setText(status);
}

// Splits the output into lines; '\r' ends a line too (progress redrawn in place).
void DownloadRow::consume(const QString &text)
{
    m_buffer += text;
    qsizetype start = 0;
    for (qsizetype i = 0; i < m_buffer.size(); ++i) {
        const QChar c = m_buffer.at(i);
        if (c != QLatin1Char('\n') && c != QLatin1Char('\r'))
            continue;
        if (i > start)
            handleLine(m_buffer.mid(start, i - start));
        start = i + 1;
    }
    m_buffer.remove(0, start);
}

void DownloadRow::handleLine(const QString &line)
{
    if (m_engine == YtDlp) {
        handleYtDlpLine(line);
        return;
    }
    const QString text = line.trimmed();
    if (text.isEmpty() || text.contains(QLatin1String("Failed to fetch secrets")))
        return; // a warning spotdl repeats on every run
    m_lastLine = text;
    if (m_engine == SpotDl && (text.contains(QLatin1String("Downloaded \"")) || text.contains(QLatin1String("Skipping"))))
        m_succeeded = true;
    if (!m_stopping)
        m_statusLabel->setText(tr("Download in corso…"));
}

void DownloadRow::handleYtDlpLine(const QString &line)
{
    if (line.startsWith(QLatin1String("DLT|"))) {
        m_expectedBytes = line.mid(4).toDouble(); // "NA" gives 0
        return;
    }
    if (line.startsWith(QLatin1String("DLF|"))) {
        m_filePath = line.mid(4);
        return;
    }
    if (line.startsWith(QLatin1String("DLP|"))) {
        const QStringList fields = line.split(QLatin1Char('|'));
        if (fields.size() < 6 || m_stopping)
            return;
        m_fileBytes = fields.at(1).toDouble();
        m_fileTotal = fields.at(2).toDouble();
        if (m_fileTotal <= 0)
            m_fileTotal = fields.at(3).toDouble();
        const double speed = fields.at(4).toDouble();

        if (m_lastUpdate.isValid() && m_lastUpdate.elapsed() < 150 && m_fileBytes < m_fileTotal)
            return;
        m_lastUpdate.start();

        // Video and audio arrive as separate files: the bar covers them all.
        const double done = m_doneBytes + m_fileBytes;
        const double all = qMax(m_expectedBytes, m_doneBytes + m_fileTotal);
        QStringList status;
        if (all > 0) {
            const double fraction = qBound(0.0, done / all, 1.0);
            m_bar->setRange(0, 1000);
            m_bar->setValue(int(fraction * 1000));
            status << QStringLiteral("%1%").arg(int(fraction * 100));
        } else {
            m_bar->setRange(0, 0);
        }
        if (speed > 0) {
            status << formatSpeed(speed);
            if (all > done)
                status << formatDuration(qMax(1, int((all - done) / speed)));
        }
        m_statusLabel->setText(status.join(QStringLiteral(" · ")));
        return;
    }
    if (line.startsWith(QLatin1String("ERROR:"))) {
        m_error = cleanError(line);
        return;
    }
    if (m_stopping)
        return;
    if (line.startsWith(QLatin1String("[download] Destination:"))) {
        m_doneBytes += qMax(m_fileBytes, m_fileTotal); // the previous file is complete
        m_fileBytes = m_fileTotal = 0;
    } else if (line.startsWith(QLatin1String("[Merger]"))) {
        setBusy(tr("Unione di video e audio…"));
    } else if (line.startsWith(QLatin1String("[ExtractAudio] Destination"))) {
        setBusy(tr("Conversione dell'audio…"));
    } else if (line.startsWith(QLatin1String("[VideoRemuxer] Remuxing"))
               || line.startsWith(QLatin1String("[VideoConvertor] Converting"))) {
        setBusy(tr("Conversione del video…"));
    } else if (line.startsWith(QLatin1Char('[')) && m_doneBytes + m_fileBytes <= 0 && m_bar->maximum() == 0) {
        m_statusLabel->setText(tr("Preparazione…"));
    }
}

void DownloadRow::reveal()
{
    const bool hasFile = !m_filePath.isEmpty() && QFileInfo::exists(m_filePath);
#if defined(Q_OS_MACOS)
    if (hasFile && QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), m_filePath}))
        return;
#elif defined(Q_OS_WIN)
    if (hasFile && QProcess::startDetached(QStringLiteral("explorer"),
                                           {QStringLiteral("/select,"), QDir::toNativeSeparators(m_filePath)}))
        return;
#endif
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(hasFile ? QFileInfo(m_filePath).absolutePath() : m_directory));
}

// ---------------------------------------------------------------------------
// SimplePage

SimplePage::SimplePage(QWidget *parent)
    : QWidget(parent)
{
    QSettings settings;

    // --- sites -------------------------------------------------------------
    m_serviceBar = new QWidget;
    auto *barLayout = new QHBoxLayout(m_serviceBar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(4);
    m_serviceGroup = new QButtonGroup(this);
    for (Service service : {Service::YouTube, Service::Spotify, Service::Instagram, Service::TikTok}) {
        auto *button = new QToolButton;
        button->setCheckable(true);
        button->setIcon(serviceIcon(service));
        button->setIconSize(QSize(28, 28));
        button->setText(serviceName(service));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumWidth(78);
        button->setStyleSheet(QStringLiteral(
            "QToolButton { border: 1px solid transparent; border-radius: 9px; padding: 5px 8px 3px 8px; }"
            "QToolButton:hover { background: rgba(128, 128, 128, 0.16); }"
            "QToolButton:checked { background: rgba(128, 128, 128, 0.22); border-color: palette(highlight); }"));
        m_serviceGroup->addButton(button, int(service));
        barLayout->addWidget(button);
    }
    connect(m_serviceGroup, &QButtonGroup::idClicked, this, [this](int id) { setService(Service(id)); });

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 6, 12, 12);
    root->setSpacing(10);

    // --- link --------------------------------------------------------------
    auto *linkRow = new QHBoxLayout;
    m_linkEdit = new QLineEdit;
    m_linkEdit->setClearButtonEnabled(true);
    m_linkEdit->setMinimumHeight(30);
    auto *addButton = new QPushButton(tr("Aggiungi"));
    addButton->setAutoDefault(false);
    linkRow->addWidget(m_linkEdit, 1);
    linkRow->addWidget(addButton);
    root->addLayout(linkRow);
    connect(addButton, &QPushButton::clicked, this, [this] { addFromText(m_linkEdit->text()); });
    connect(m_linkEdit, &QLineEdit::returnPressed, this, [this] { addFromText(m_linkEdit->text()); });
    // A pasted link is added right away; a typed one waits for Enter.
    connect(m_linkEdit, &QLineEdit::textEdited, this, [this](const QString &text) {
        const qsizetype grown = text.size() - m_linkLength;
        m_linkLength = text.size();
        if (grown >= 8 && allUrls(splitTokens(text)))
            QTimer::singleShot(0, this, [this] { addFromText(m_linkEdit->text()); });
    });
    // Paste anywhere in the page, without clicking the field first.
    auto *paste = new QShortcut(QKeySequence::Paste, this);
    paste->setContext(Qt::WidgetWithChildrenShortcut);
    connect(paste, &QShortcut::activated, this, [this] { addFromText(QGuiApplication::clipboard()->text()); });

    // --- format and quality ------------------------------------------------
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(3, 1);
    const auto optionRow = [&](int row, const QString &title, const QStringList &formats, int steps,
                               QCheckBox *&check, QComboBox *&format, QSlider *&slider, QLabel *&value) {
        check = new QCheckBox(title);
        format = new QComboBox;
        format->addItems(formats);
        format->setMinimumWidth(84);
        slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, steps - 1);
        slider->setPageStep(1);
        slider->setTickPosition(QSlider::TicksBelow);
        slider->setTickInterval(1);
        value = new QLabel;
        value->setMinimumWidth(96);
        grid->addWidget(check, row, 0);
        grid->addWidget(format, row, 1);
        grid->addWidget(new QLabel(tr("Qualità")), row, 2);
        grid->addWidget(slider, row, 3);
        grid->addWidget(value, row, 4);
        connect(check, &QCheckBox::toggled, this, &SimplePage::updateOptionStates);
        connect(format, &QComboBox::currentIndexChanged, this, &SimplePage::updateOptionStates);
        connect(slider, &QSlider::valueChanged, this, &SimplePage::updateOptionStates);
    };
    optionRow(0, tr("Video"), kVideoFormats, kHeights.size(), m_videoCheck, m_videoFormat, m_videoQuality,
              m_videoQualityLabel);
    optionRow(1, tr("Audio"), kAudioFormats, kBitrates.size(), m_audioCheck, m_audioFormat, m_audioQuality,
              m_audioQualityLabel);
    m_videoCheck->setToolTip(tr("Vale per i prossimi link aggiunti; ogni riga della lista si può poi cambiare"));
    m_audioCheck->setToolTip(m_videoCheck->toolTip());
    m_audioFormat->setToolTip(tr("Formato dei download solo audio; insieme al video l'audio resta nel file video"));
    root->addLayout(grid);

    {
        const QSignalBlocker b1(m_videoCheck), b2(m_audioCheck), b3(m_videoFormat), b4(m_audioFormat),
            b5(m_videoQuality), b6(m_audioQuality);
        m_videoCheck->setChecked(settings.value(QStringLiteral("simple/video"), true).toBool());
        m_audioCheck->setChecked(settings.value(QStringLiteral("simple/audio"), true).toBool()
                                 || !m_videoCheck->isChecked());
        m_videoFormat->setCurrentIndex(
            qMax(0, kVideoFormats.indexOf(settings.value(QStringLiteral("simple/videoFormat")).toString())));
        m_audioFormat->setCurrentIndex(
            qMax(0, kAudioFormats.indexOf(settings.value(QStringLiteral("simple/audioFormat")).toString())));
        const int height = kHeights.indexOf(settings.value(QStringLiteral("simple/height"), 1080).toInt());
        const int bitrate = kBitrates.indexOf(settings.value(QStringLiteral("simple/bitrate"), 192).toInt());
        m_videoQuality->setValue(height < 0 ? kHeights.indexOf(1080) : height);
        m_audioQuality->setValue(bitrate < 0 ? kBitrates.indexOf(192) : bitrate);
    }
    // One of the two always stays on.
    connect(m_videoCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (!on && !m_audioCheck->isChecked())
            m_audioCheck->setChecked(true);
    });
    connect(m_audioCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (!on && !m_videoCheck->isChecked())
            m_videoCheck->setChecked(true);
    });

    // --- list --------------------------------------------------------------
    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *host = new QWidget;
    auto *hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    m_emptyLabel = new QLabel(tr("Incolla un link: ogni link diventa una riga di questa lista.\n"
                                 "Una playlist aggiunge tutti i suoi elementi."));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setContentsMargins(20, 60, 20, 20);
    QPalette faded = m_emptyLabel->palette();
    faded.setColor(QPalette::WindowText, faded.color(QPalette::PlaceholderText));
    m_emptyLabel->setPalette(faded);
    m_rowsLayout = new QVBoxLayout;
    m_rowsLayout->setSpacing(0);
    hostLayout->addWidget(m_emptyLabel);
    hostLayout->addLayout(m_rowsLayout);
    hostLayout->addStretch(1);
    m_scroll->setWidget(host);
    root->addWidget(m_scroll, 1);

    // --- folder and actions ------------------------------------------------
    auto *dirRow = new QHBoxLayout;
    m_dirEdit = new QLineEdit(settings.value(QStringLiteral("simple/dir"), downloadsDir()).toString());
    auto *browse = new QPushButton(tr("Sfoglia…"));
    browse->setAutoDefault(false);
    dirRow->addWidget(new QLabel(tr("Cartella")));
    dirRow->addWidget(m_dirEdit, 1);
    dirRow->addWidget(browse);
    root->addLayout(dirRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Cartella di destinazione"), directory());
        if (!dir.isEmpty()) {
            m_dirEdit->setText(dir);
            QSettings().setValue(QStringLiteral("simple/dir"), dir);
        }
    });
    connect(m_dirEdit, &QLineEdit::editingFinished, this,
            [this] { QSettings().setValue(QStringLiteral("simple/dir"), m_dirEdit->text()); });

    auto *actions = new QHBoxLayout;
    m_summaryLabel = new QLabel;
    m_clearButton = new QPushButton(tr("Svuota lista"));
    m_clearButton->setAutoDefault(false);
    m_downloadAllButton = new QPushButton(tr("Scarica tutti"));
    m_downloadAllButton->setAutoDefault(false);
    actions->addWidget(m_summaryLabel, 1);
    actions->addWidget(m_clearButton);
    actions->addWidget(m_downloadAllButton);
    root->addLayout(actions);
    connect(m_clearButton, &QPushButton::clicked, this, &SimplePage::clearAll);
    connect(m_downloadAllButton, &QPushButton::clicked, this, &SimplePage::downloadAll);

    // Closing the window stops what is running, and waits for it to end.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        const QList<QProcess *> processes = findChildren<QProcess *>();
        for (QProcess *process : processes) {
            process->disconnect();
            stopTool(process);
        }
        for (QProcess *process : processes)
            process->waitForFinished(1500);
    });

    setService(Service(qBound(0, settings.value(QStringLiteral("simple/service")).toInt(), 3)));
    updateSummary();
}

void SimplePage::setService(Service service)
{
    m_service = service;
    m_serviceGroup->button(int(service))->setChecked(true);
    switch (service) {
    case Service::YouTube:
        m_linkEdit->setPlaceholderText(tr("Incolla il link di un video o di una playlist"));
        break;
    case Service::Spotify:
        m_linkEdit->setPlaceholderText(
            tr("Incolla il link di un brano, un album o una playlist, oppure scrivi titolo e artista"));
        break;
    case Service::Instagram:
        m_linkEdit->setPlaceholderText(tr("Incolla il link di un post o di un reel, oppure scrivi il nome di un profilo"));
        break;
    case Service::TikTok:
        m_linkEdit->setPlaceholderText(tr("Incolla il link di un video TikTok"));
        break;
    }
    updateOptionStates();
}

DownloadSettings SimplePage::currentSettings(Service service) const
{
    DownloadSettings s;
    s.videoFormat = m_videoFormat->currentText();
    s.height = kHeights.value(m_videoQuality->value(), 1080);
    s.audioFormat = m_audioFormat->currentText();
    s.bitrate = kBitrates.value(m_audioQuality->value(), 192);
    if (service == Service::Spotify || !m_videoCheck->isChecked())
        s.mode = DownloadSettings::AudioOnly;
    else if (!m_audioCheck->isChecked())
        s.mode = DownloadSettings::VideoOnly;
    return s;
}

void SimplePage::updateOptionStates()
{
    const bool music = m_service == Service::Spotify; // audio only
    const bool video = !music && m_videoCheck->isChecked();
    const bool audio = music || m_audioCheck->isChecked();
    m_videoCheck->setEnabled(!music);
    m_audioCheck->setEnabled(!music);
    m_videoFormat->setEnabled(video);
    m_videoQuality->setEnabled(video);
    m_videoQualityLabel->setEnabled(video);
    m_audioFormat->setEnabled(audio);

    const int height = kHeights.value(m_videoQuality->value());
    m_videoQualityLabel->setText(height > 0 ? QStringLiteral("%1p").arg(height) : tr("Massima"));
    const bool lossless = isLossless(m_audioFormat->currentText());
    m_audioQuality->setEnabled(audio && !lossless);
    m_audioQualityLabel->setEnabled(audio);
    m_audioQualityLabel->setText(lossless ? tr("Senza perdita")
                                          : QStringLiteral("%1 kbps").arg(kBitrates.value(m_audioQuality->value())));
    saveOptions();
}

void SimplePage::saveOptions()
{
    QSettings settings;
    settings.setValue(QStringLiteral("simple/service"), int(m_service));
    settings.setValue(QStringLiteral("simple/video"), m_videoCheck->isChecked());
    settings.setValue(QStringLiteral("simple/audio"), m_audioCheck->isChecked());
    settings.setValue(QStringLiteral("simple/videoFormat"), m_videoFormat->currentText());
    settings.setValue(QStringLiteral("simple/audioFormat"), m_audioFormat->currentText());
    settings.setValue(QStringLiteral("simple/height"), kHeights.value(m_videoQuality->value()));
    settings.setValue(QStringLiteral("simple/bitrate"), kBitrates.value(m_audioQuality->value()));
}

QString SimplePage::directory() const
{
    const QString dir = m_dirEdit->text().trimmed();
    if (dir.isEmpty())
        return downloadsDir();
    if (dir == QLatin1String("~") || dir.startsWith(QLatin1String("~/")))
        return QDir::homePath() + dir.mid(1);
    return dir;
}

// ---------------------------------------------------------------------------
// Adding links

void SimplePage::addFromText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;
    const QStringList tokens = splitTokens(trimmed);
    static const QRegularExpression profileRe(QStringLiteral("^[@#]?[\\w.]+$"));

    if (allUrls(tokens)) {
        Service last = m_service;
        for (QString url : tokens) {
            if (!url.contains(QLatin1String("://")))
                url.prepend(QLatin1String("https://"));
            const QString host = QUrl(url).host().toLower();
            // Any other site goes to yt-dlp, which knows many more.
            Service service = m_service == Service::TikTok ? Service::TikTok : Service::YouTube;
            if (host.contains(QLatin1String("spotify.")))
                service = Service::Spotify;
            else if (host.contains(QLatin1String("instagram.")) || host.contains(QLatin1String("instagr.am")))
                service = Service::Instagram;
            else if (host.contains(QLatin1String("tiktok.")))
                service = Service::TikTok;
            else if (host.contains(QLatin1String("youtube.")) || host.contains(QLatin1String("youtu.be")))
                service = Service::YouTube;
            addTarget(service, url);
            last = service;
        }
        if (last != m_service)
            setService(last); // the buttons follow what was pasted
    } else if (m_service == Service::Spotify) {
        addTarget(Service::Spotify, trimmed); // a search: "title artist"
    } else if (m_service == Service::Instagram
               && std::all_of(tokens.cbegin(), tokens.cend(),
                              [](const QString &t) { return profileRe.match(t).hasMatch(); })) {
        for (QString name : tokens)
            addTarget(Service::Instagram, name.startsWith(QLatin1Char('@')) ? name.mid(1) : name);
    } else {
        QToolTip::showText(m_linkEdit->mapToGlobal(QPoint(8, m_linkEdit->height())),
                           tr("Serve un link completo, per esempio https://…"), m_linkEdit);
        return;
    }
    m_linkEdit->clear();
    m_linkLength = 0;
}

void SimplePage::addTarget(Service service, const QString &target)
{
    const DownloadSettings settings = currentSettings(service);

    if (service == Service::Spotify) {
        resolveWithSpotDl(addRow(service, DownloadRow::SpotDl, target, settings), target);
        return;
    }
    if (service == Service::Instagram) {
        // Videos go through yt-dlp (format, quality, progress); photos and
        // whole profiles through instaloader.
        QStringList profiles, posts;
        instagramTargets({target}, profiles, posts);
        if (!posts.isEmpty()) {
            resolveWithYtDlp(addRow(service, DownloadRow::YtDlp, target, settings), target, posts.first());
        } else if (!profiles.isEmpty()) {
            const QString profile = profiles.first();
            DownloadRow *row = addRow(service, DownloadRow::Instaloader, profile, settings);
            row->setInfo(profile.startsWith(QLatin1Char('#')) ? profile : QLatin1Char('@') + profile,
                         profile.startsWith(QLatin1Char('#')) ? tr("Hashtag Instagram") : tr("Profilo Instagram"), 0);
        }
        return;
    }
    resolveWithYtDlp(addRow(service, DownloadRow::YtDlp, target, settings), target, {});
}

DownloadRow *SimplePage::addRow(Service service, DownloadRow::Engine engine, const QString &target,
                                const DownloadSettings &settings, int index)
{
    auto *row = new DownloadRow(service, engine, target, settings);
    const bool pasted = index < 0; // the entries of a playlist do not move the view
    if (pasted || index > m_rows.size())
        index = m_rows.size();
    m_rows.insert(index, row);
    m_rowsLayout->insertWidget(index, row);
    connect(row, &DownloadRow::downloadRequested, this, [row] { row->setQueued(); });
    connect(row, &DownloadRow::removeRequested, this, [this, row] { removeRow(row); });
    connect(row, &DownloadRow::stateChanged, this, [this] {
        updateSummary();
        schedulePump();
    });
    updateSummary();
    if (pasted)
        QTimer::singleShot(0, row, [this, row] { m_scroll->ensureWidgetVisible(row, 0, 0); });
    return row;
}

void SimplePage::removeRow(DownloadRow *row)
{
    if (!m_rows.removeOne(row))
        return;
    m_rowsLayout->removeWidget(row);
    row->hide();
    row->deleteLater(); // its destructor stops what it was running
    updateSummary();
    schedulePump();
}

// Reads title and duration; a playlist yields one line per entry, so its rows
// appear while the list is still being read.
void SimplePage::resolveWithYtDlp(DownloadRow *row, const QString &url, const QString &instagramPost)
{
    const QString program = findTool(QStringLiteral("yt-dlp"));
    if (program.isEmpty()) {
        row->setFailed(tr("yt-dlp non trovato"));
        return;
    }

    struct Pending {
        QByteArray buffer;
        int count = 0;
        QPointer<DownloadRow> last;
    };
    auto pending = std::make_shared<Pending>();
    const auto takeLines = [this, row, pending](bool flush) {
        while (true) {
            qsizetype end = pending->buffer.indexOf('\n');
            if (end < 0 && flush && !pending->buffer.isEmpty())
                end = pending->buffer.size();
            if (end < 0)
                return;
            const QJsonObject entry = QJsonDocument::fromJson(pending->buffer.left(end)).object();
            pending->buffer.remove(0, end + 1);

            QString link = entry.value(QLatin1String("webpage_url")).toString();
            if (link.isEmpty())
                link = entry.value(QLatin1String("url")).toString();
            QString title = entry.value(QLatin1String("title")).toString();
            // Entries of a playlist without a title are removed or private videos.
            if (link.isEmpty() || (title.isEmpty() && entry.contains(QLatin1String("playlist_title"))))
                continue;
            if (title.isEmpty())
                title = link;
            QString author = entry.value(QLatin1String("channel")).toString();
            if (author.isEmpty())
                author = entry.value(QLatin1String("uploader")).toString();
            const int seconds = int(entry.value(QLatin1String("duration")).toDouble());

            DownloadRow *target = row;
            if (pending->count++ > 0) {
                const int after = m_rows.indexOf(pending->last ? pending->last.data() : row);
                target = addRow(row->service(), DownloadRow::YtDlp, link, row->settings(), after + 1);
            }
            target->setTarget(DownloadRow::YtDlp, link);
            target->setInfo(title, author, seconds);
            pending->last = target;
        }
    };

    auto *process = new QProcess(row); // stopped with the row
    process->setProperty("reading", true);
    process->setProcessEnvironment(toolEnvironment());
    connect(process, &QProcess::readyReadStandardOutput, row, [process, pending, takeLines] {
        pending->buffer += process->readAllStandardOutput();
        takeLines(false);
    });
    connect(process, &QProcess::finished, row, [=] {
        pending->buffer += process->readAllStandardOutput();
        takeLines(true);
        const QString error = cleanError(QString::fromUtf8(process->readAllStandardError()));
        process->setProperty("reading", false);
        process->deleteLater();
        updateSummary();
        if (pending->count > 0)
            return;
        if (!instagramPost.isEmpty()) {
            // Not a video: a post with photos, which instaloader can fetch.
            row->setTarget(DownloadRow::Instaloader, instagramPost);
            row->setInfo(tr("Post Instagram %1").arg(instagramPost.mid(1)), tr("Foto e video del post"), 0);
            return;
        }
        row->setFailed(error.isEmpty() ? tr("Link non riconosciuto") : error);
    });
    connect(process, &QProcess::errorOccurred, row, [row, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            row->setFailed(tr("Impossibile avviare yt-dlp"));
        }
    });
    process->start(program, {QStringLiteral("--flat-playlist"),
                             wantsWholePlaylist(url) ? QStringLiteral("--yes-playlist") : QStringLiteral("--no-playlist"),
                             QStringLiteral("--no-warnings"), QStringLiteral("--no-colors"), QStringLiteral("--print"),
                             QStringLiteral("%(.{title,webpage_url,url,duration,playlist_title,channel,uploader})j"),
                             url});
    process->closeWriteChannel();
}

// spotdl writes the tracks of a link (or the best match of a search) to a file.
void SimplePage::resolveWithSpotDl(DownloadRow *row, const QString &target)
{
    const QString program = findTool(QStringLiteral("spotdl"));
    if (program.isEmpty() || !m_tempDir.isValid()) {
        row->setFailed(tr("spotdl non trovato"));
        return;
    }
    const QString file = m_tempDir.filePath(QStringLiteral("%1.spotdl").arg(++m_tempCounter));

    auto *process = new QProcess(row);
    process->setProcessEnvironment(toolEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(m_tempDir.path());
    connect(process, &QProcess::finished, row, [this, row, process, file] {
        process->deleteLater();
        QFile saved(file);
        QJsonArray songs;
        if (saved.open(QIODevice::ReadOnly))
            songs = QJsonDocument::fromJson(saved.readAll()).array();
        saved.remove();

        DownloadRow *last = nullptr;
        for (const QJsonValue &value : std::as_const(songs)) {
            const QJsonObject song = value.toObject();
            const QString link = song.value(QLatin1String("url")).toString();
            if (link.isEmpty())
                continue;
            QStringList artists;
            for (const QJsonValue &artist : song.value(QLatin1String("artists")).toArray())
                artists << artist.toString();
            DownloadRow *entry = row;
            if (last)
                entry = addRow(Service::Spotify, DownloadRow::SpotDl, link, row->settings(), m_rows.indexOf(last) + 1);
            entry->setTarget(DownloadRow::SpotDl, link);
            entry->setInfo(song.value(QLatin1String("name")).toString(), artists.join(QStringLiteral(", ")),
                           song.value(QLatin1String("duration")).toInt());
            last = entry;
        }
        if (!last)
            row->setFailed(tr("Nessun brano trovato"));
    });
    connect(process, &QProcess::errorOccurred, row, [row, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            row->setFailed(tr("Impossibile avviare spotdl"));
        }
    });
    process->start(program, {QStringLiteral("save"), target, QStringLiteral("--save-file"), file});
    process->closeWriteChannel();
}

// ---------------------------------------------------------------------------
// Queue

void SimplePage::downloadAll()
{
    for (DownloadRow *row : std::as_const(m_rows))
        row->setQueued(); // only the rows that are waiting or failed
}

void SimplePage::clearAll()
{
    const bool running = std::any_of(m_rows.cbegin(), m_rows.cend(),
                                     [](const DownloadRow *row) { return row->state() == DownloadRow::Running; });
    if (running
        && QMessageBox::question(this, tr("Svuota lista"),
                                 tr("Ci sono download in corso. Fermarli e svuotare la lista?"))
               != QMessageBox::Yes)
        return;
    const QList<DownloadRow *> rows = m_rows;
    for (DownloadRow *row : rows)
        removeRow(row);
}

void SimplePage::schedulePump()
{
    if (m_pumpScheduled)
        return;
    m_pumpScheduled = true;
    QTimer::singleShot(0, this, &SimplePage::pump);
}

// Starts the queued rows, a few at a time, in list order.
void SimplePage::pump()
{
    m_pumpScheduled = false;
    int running = 0;
    for (const DownloadRow *row : std::as_const(m_rows))
        running += row->state() == DownloadRow::Running;
    for (DownloadRow *row : std::as_const(m_rows)) {
        if (running >= kMaxParallel)
            break;
        if (row->state() != DownloadRow::Queued)
            continue;
        row->start(directory());
        running += row->state() == DownloadRow::Running;
    }
}

void SimplePage::updateSummary()
{
    int waiting = 0, active = 0, done = 0;
    bool reading = false; // a playlist whose entries are still arriving
    for (const DownloadRow *row : std::as_const(m_rows)) {
        const QList<QProcess *> processes = row->findChildren<QProcess *>(Qt::FindDirectChildrenOnly);
        for (const QProcess *process : processes)
            reading = reading || process->property("reading").toBool();
        switch (row->state()) {
        case DownloadRow::Ready:
        case DownloadRow::Failed:
            ++waiting;
            break;
        case DownloadRow::Queued:
        case DownloadRow::Running:
            ++active;
            break;
        case DownloadRow::Done:
            ++done;
            break;
        case DownloadRow::Resolving:
            break;
        }
    }
    QStringList parts;
    if (!m_rows.isEmpty())
        parts << (m_rows.size() == 1 ? tr("1 elemento") : tr("%1 elementi").arg(m_rows.size()));
    if (active > 0)
        parts << tr("%1 in corso").arg(active);
    if (done > 0)
        parts << (done == 1 ? tr("1 completato") : tr("%1 completati").arg(done));
    if (reading)
        parts << tr("lettura dei link in corso…");
    m_summaryLabel->setText(parts.join(QStringLiteral("  ·  ")));
    m_emptyLabel->setVisible(m_rows.isEmpty());
    m_clearButton->setEnabled(!m_rows.isEmpty());
    m_downloadAllButton->setEnabled(waiting > 0);
}
