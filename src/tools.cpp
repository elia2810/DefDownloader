#include "tools.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

// yt-dlp --help layout:
//   "  Section Name:"                           two spaces
//   "    -f, --format FORMAT     Description"   four spaces, starts with '-'
//   "                            continuation"  deeper indent
QList<OptionSection> parseYtDlpHelp(const QString &text, QStringList *unparsed)
{
    static const QRegularExpression sectionRe(QStringLiteral("^  (\\S.*):$"));
    static const QRegularExpression optionRe(QStringLiteral("^    (-\\S.*?)(?:\\s{2,}(\\S.*))?$"));
    static const QRegularExpression specRe(
        QStringLiteral("^(-[\\w-]+(?:, -[\\w-]+)*)(?:\\s+(.+))?$"));

    QList<OptionSection> sections;
    bool inOption = false;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        while (line.endsWith(QLatin1Char('\r')) || line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        if (line.isEmpty()) {
            inOption = false;
            continue;
        }

        if (const auto m = sectionRe.match(line); m.hasMatch()) {
            sections.append(OptionSection{m.captured(1), {}});
            inOption = false;
            continue;
        }
        if (sections.isEmpty())
            continue; // "Usage:" / "Options:" preamble

        if (const auto m = optionRe.match(line); m.hasMatch()) {
            const QString spec = m.captured(1).trimmed();
            const auto sm = specRe.match(spec);
            if (!sm.hasMatch()) {
                if (unparsed)
                    unparsed->append(line);
                inOption = false;
                continue;
            }
            OptionDef def;
            def.description = m.captured(2);
            if (sections.last().title.startsWith(QLatin1String("Preset"))) {
                // "-t mp3": a complete, ready-made argument pair.
                def.label = spec;
                def.args = spec.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            } else {
                def.label = sm.captured(1);
                def.args = {sm.captured(1).split(QStringLiteral(", ")).last()};
                def.argName = sm.captured(2);
                def.hasValue = !def.argName.isEmpty();
            }
            sections.last().options.append(def);
            inOption = true;
            continue;
        }

        if (inOption && line.startsWith(QLatin1String("     "))) {
            QString &desc = sections.last().options.last().description;
            if (!desc.isEmpty())
                desc += QLatin1Char(' ');
            desc += line.trimmed();
            continue;
        }
        if (line.startsWith(QLatin1String("    ")))
            continue; // free text inside a section
        if (unparsed)
            unparsed->append(line);
    }

    sections.removeIf([](const OptionSection &s) { return s.options.isEmpty(); });
    return sections;
}

// ffmpeg -h long layout:
//   "Section title:"                                   column 0
//   "-name[:<stream_spec>] <arg>  description"         column 0, starts with '-'
QList<OptionSection> parseFfmpegHelp(const QString &text, QStringList *unparsed)
{
    static const QRegularExpression optionRe(QStringLiteral(
        "^(-{1,2}[^\\s\\[<]+)(\\[:<[^>]*>\\])?(?: (<[^>]*>))?(?:\\s+(.*))?$"));
    // Listed among the generic per-stream options, but they act on the input file.
    static const QSet<QString> inputByDefault = {
        QStringLiteral("-hwaccel"), QStringLiteral("-hwaccel_device"),
        QStringLiteral("-hwaccel_output_format"), QStringLiteral("-autorotate"),
        QStringLiteral("-display_rotation"), QStringLiteral("-display_hflip"),
        QStringLiteral("-display_vflip"), QStringLiteral("-apply_cropping"),
        QStringLiteral("-guess_layout_max"), QStringLiteral("-canvas_size"),
        QStringLiteral("-fix_sub_duration"), QStringLiteral("-discard"),
    };

    QList<OptionSection> sections;
    OptionDef::Position position = OptionDef::Global;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        while (line.endsWith(QLatin1Char('\r')) || line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        if (line.isEmpty() || line.startsWith(QLatin1Char(' ')))
            continue;

        if (!line.startsWith(QLatin1Char('-'))) {
            if (line.endsWith(QLatin1Char(':'))) {
                line.chop(1);
                const QString lower = line.toLower();
                if (lower.contains(QLatin1String("global")) || lower.contains(QLatin1String("information"))
                    || lower.contains(QLatin1String("help")))
                    position = OptionDef::Global;
                else if (lower.contains(QLatin1String("input-only")))
                    position = OptionDef::Input;
                else if (lower.contains(QLatin1String("output-only")))
                    position = OptionDef::Output;
                else
                    position = OptionDef::Either;
                sections.append(OptionSection{line, {}});
            }
            continue; // usage text and notes
        }
        if (sections.isEmpty())
            continue;

        const auto m = optionRe.match(line);
        if (!m.hasMatch()) {
            if (unparsed)
                unparsed->append(line);
            continue;
        }
        OptionDef def;
        def.label = m.captured(1);
        def.args = {m.captured(1)};
        def.streamSpec = !m.captured(2).isEmpty();
        def.argName = m.captured(3).mid(1).chopped(m.captured(3).isEmpty() ? 0 : 1);
        // The help omits the argument name for several options that do take one,
        // so every ffmpeg option gets an optional value field.
        def.hasValue = true;
        def.valueOptional = true;
        def.position = position;
        def.defaultInput = inputByDefault.contains(def.label);
        def.description = m.captured(4);
        sections.last().options.append(def);
    }

    sections.removeIf([](const OptionSection &s) { return s.options.isEmpty(); });
    return sections;
}

// argparse layout:
//   "Section title:"                              column 0
//   "  -o, --output OUTPUT   Description"          two spaces, starts with '-'
//   "  --format {mp3,flac}"                        choices in braces
//   "  --audio [{a,b} ...]"                        brackets: optional, "...": several values
//   "                        continuation"         deeper indent
// Lines with two spaces that do not start with '-' are positional arguments
// or free text, and are skipped together with their continuation lines.
QList<OptionSection> parseArgparseHelp(const QString &text, QStringList *unparsed)
{
    static const QRegularExpression optionRe(QStringLiteral("^  (-\\S.*?)(?:\\s{2,}(\\S.*))?$"));
    static const QRegularExpression partRe(QStringLiteral("^(-{1,2}\\w[\\w-]*)(?:\\s+(.+))?$"));

    QList<OptionSection> sections;
    bool inOption = false;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        while (line.endsWith(QLatin1Char('\r')) || line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        if (line.isEmpty()) {
            inOption = false;
            continue;
        }
        if (!line.startsWith(QLatin1Char(' '))) {
            inOption = false;
            if (line.endsWith(QLatin1Char(':')) && !line.startsWith(QLatin1String("usage"))) {
                line.chop(1);
                line[0] = line.at(0).toUpper();
                sections.append(OptionSection{line, {}});
            }
            continue;
        }
        if (sections.isEmpty())
            continue; // usage block

        if (line.startsWith(QLatin1String("  -"))) {
            inOption = false;
            const auto m = optionRe.match(line);
            QStringList names;
            QString metavar;
            bool valid = m.hasMatch();
            if (valid) {
                // "-o OUTPUT, --output OUTPUT": every name may repeat the value name.
                for (const QString &part : m.captured(1).split(QStringLiteral(", "))) {
                    const auto pm = partRe.match(part);
                    if (!pm.hasMatch()) {
                        valid = false;
                        break;
                    }
                    names.append(pm.captured(1));
                    if (!pm.captured(2).isEmpty())
                        metavar = pm.captured(2);
                }
            }
            if (!valid) {
                if (unparsed)
                    unparsed->append(line);
                continue;
            }

            OptionDef def;
            def.label = names.join(QStringLiteral(", "));
            def.args = {names.first()};
            for (const QString &name : std::as_const(names)) {
                if (name.startsWith(QLatin1String("--"))) {
                    def.args = {name};
                    break;
                }
            }
            if (!metavar.isEmpty()) {
                def.hasValue = true;
                if (metavar.startsWith(QLatin1Char('[')) && metavar.endsWith(QLatin1Char(']'))) {
                    def.valueOptional = true;
                    metavar = metavar.mid(1, metavar.size() - 2);
                }
                if (metavar.endsWith(QLatin1String(" ..."))) {
                    def.multiValue = true;
                    metavar.chop(4);
                }
                if (metavar.startsWith(QLatin1Char('{')) && metavar.endsWith(QLatin1Char('}'))) {
                    def.choices = metavar.mid(1, metavar.size() - 2).split(QLatin1Char(','));
                    metavar = def.choices.join(QStringLiteral(" | "));
                }
                def.argName = metavar;
            }
            def.description = m.captured(2);
            sections.last().options.append(def);
            inOption = true;
            continue;
        }

        if (inOption && line.startsWith(QLatin1String("      "))) {
            QString &desc = sections.last().options.last().description;
            if (!desc.isEmpty())
                desc += QLatin1Char(' ');
            desc += line.trimmed();
            continue;
        }
        inOption = false;
    }

    sections.removeIf([](const OptionSection &s) { return s.options.isEmpty(); });
    return sections;
}

QList<ListEntry> parseFfmpegList(const QString &text, int nameColumn, bool hasSeparator,
                                 int headerLines, int flagWidth)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    QList<ListEntry> entries;
    bool started = !hasSeparator;
    int skipped = 0;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty())
            continue;
        if (!started) {
            if (trimmed.size() >= 3 && trimmed.count(QLatin1Char('-')) == trimmed.size())
                started = true;
            continue;
        }
        if (!hasSeparator && skipped < headerLines) {
            ++skipped;
            continue;
        }

        QString flags;
        QString rest = trimmed;
        if (flagWidth > 0) {
            flags = raw.left(flagWidth).trimmed();
            rest = raw.mid(flagWidth).trimmed();
        }
        const QStringList parts = rest.split(spaces, Qt::SkipEmptyParts);
        if (parts.size() <= nameColumn)
            continue;
        if (flagWidth == 0 && nameColumn > 0)
            flags = parts.first();
        const QString description = parts.mid(nameColumn + 1).join(QLatin1Char(' '));
        // Formats list aliases as "mov,mp4,m4a".
        for (const QString &name : parts.at(nameColumn).split(QLatin1Char(','), Qt::SkipEmptyParts))
            entries.append(ListEntry{name, flags, description});
    }
    return entries;
}

// Where to look after PATH: a GUI app does not inherit the shell PATH, so the
// usual install folders are listed explicitly. Copies shipped inside the app
// come last, so a version installed (and kept up to date) by the user wins.
static QStringList extraToolDirs()
{
    QStringList dirs = {
#ifndef Q_OS_WIN
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/local/bin"),
        QDir::homePath() + QStringLiteral("/.local/bin"),
        QDir::homePath() + QStringLiteral("/bin"),
#endif
        QCoreApplication::applicationDirPath(),
    };
    return dirs;
}

QString findTool(const QString &name)
{
    const QString saved = QSettings().value(QStringLiteral("paths/") + name).toString();
    if (!saved.isEmpty() && QFileInfo(saved).isExecutable())
        return saved;
    const QString inPath = QStandardPaths::findExecutable(name);
    if (!inPath.isEmpty())
        return inPath;
    return QStandardPaths::findExecutable(name, extraToolDirs());
}

// instaloader takes profile names and post shortcodes, not links: pasted
// Instagram URLs are translated. Posts go after "--" as "-SHORTCODE".
void instagramTargets(const QStringList &inputs, QStringList &profiles, QStringList &posts)
{
    static const QRegularExpression urlRe(QStringLiteral("^(?:https?://)?(?:www\\.)?instagram\\.com/([^?#]*)"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QStringList postKinds = {QStringLiteral("p"), QStringLiteral("reel"), QStringLiteral("reels"),
                                          QStringLiteral("tv")};
    for (const QString &input : inputs) {
        const auto match = urlRe.match(input);
        if (!match.hasMatch()) {
            profiles << input;
            continue;
        }
        const QStringList path = match.captured(1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (path.isEmpty())
            continue;
        if (postKinds.contains(path.at(0)) && path.size() >= 2)
            posts << QLatin1Char('-') + path.at(1);
        else if (path.size() >= 3 && postKinds.contains(path.at(1))) // instagram.com/user/p/CODE
            posts << QLatin1Char('-') + path.at(2);
        else if (path.at(0) == QLatin1String("stories") && path.size() >= 2)
            profiles << path.at(1);
        else if (path.at(0) == QLatin1String("explore") && path.size() >= 3 && path.at(1) == QLatin1String("tags"))
            profiles << QLatin1Char('#') + path.at(2);
        else
            profiles << path.at(0);
    }
}

QProcessEnvironment toolEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Same order as findTool(), so yt-dlp picks the ffmpeg that the ffmpeg tab uses.
    QStringList path;
    for (const QString &tool : {QStringLiteral("ffmpeg"), QStringLiteral("yt-dlp"), QStringLiteral("spotdl"),
                                QStringLiteral("instaloader")}) {
        const QString saved = QSettings().value(QStringLiteral("paths/") + tool).toString();
        if (!saved.isEmpty())
            path.append(QFileInfo(saved).absolutePath());
    }
    path.append(env.value(QStringLiteral("PATH")));
    path.append(extraToolDirs());
    env.insert(QStringLiteral("PATH"), path.join(QDir::listSeparator()));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    return env;
}

QString runTool(const QString &program, const QStringList &args, int timeoutMs)
{
    QProcess process;
    process.setProcessEnvironment(toolEnvironment());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
    }
    return QString::fromUtf8(process.readAll());
}

QString downloadsDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return dir.isEmpty() ? QDir::homePath() : dir;
}

QString shellQuote(const QString &arg)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (arg.isEmpty())
        return QStringLiteral("''");
    if (safe.match(arg).hasMatch())
        return arg;
    QString quoted = arg;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// The page of the latest release redirects to ".../releases/tag/v1.2.3": the
// address it points to is all that is read. curl comes with macOS, with
// Windows 10 and later and with nearly every Linux system, so the app needs
// no network library of its own.
void fetchLatestVersion(QObject *context, std::function<void(const QString &)> done)
{
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty())
        return;
    auto *process = new QProcess(context);
    QObject::connect(process, &QProcess::finished, context, [process, done] {
        static const QRegularExpression tag(QStringLiteral("location:\\s*\\S*/releases/tag/v?(\\d+(?:\\.\\d+)+)"),
                                            QRegularExpression::CaseInsensitiveOption);
        const auto match = tag.match(QString::fromUtf8(process->readAllStandardOutput()));
        process->deleteLater();
        if (match.hasMatch())
            done(match.captured(1));
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    process->start(curl, {QStringLiteral("--silent"), QStringLiteral("--head"), QStringLiteral("--max-time"),
                          QStringLiteral("10"), kReleasesUrl});
    process->closeWriteChannel();
}

QString formatDuration(int seconds)
{
    if (seconds <= 0)
        return {};
    const int h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

void stopTool(QProcess *process)
{
    if (!process || process->state() == QProcess::NotRunning)
        return;
#ifdef Q_OS_WIN
    // Console programs ignore the close message, and ending only the process
    // would leave its children running (ffmpeg, the Python unpacked by the
    // standalone builds): the whole tree is ended.
    QProcess killer;
    killer.setProgram(QStringLiteral("taskkill"));
    killer.setArguments({QStringLiteral("/PID"), QString::number(process->processId()), QStringLiteral("/T"),
                         QStringLiteral("/F")});
    killer.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; }); // CREATE_NO_WINDOW
    killer.startDetached();
#else
    // The standalone builds forward the signal to the Python they unpack.
    process->terminate();
    QTimer::singleShot(3000, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
#endif
}

void abandonTool(QProcess *process)
{
    if (!process)
        return;
    process->disconnect();
    process->setParent(QCoreApplication::instance());
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (process->state() == QProcess::NotRunning)
        process->deleteLater();
    else
        stopTool(process);
}
