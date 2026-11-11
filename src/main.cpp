#include "tools.h"
#include "tooltab.h"

#include <QApplication>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>

// `ytmlp --selftest`: parses the help of the installed tools and prints what
// was understood, without opening a window.
static int selfTest()
{
    QTextStream out(stdout);
    for (const QString &tool : {QStringLiteral("yt-dlp"), QStringLiteral("ffmpeg"), QStringLiteral("spotdl"),
                                QStringLiteral("instaloader")}) {
        const QString program = findTool(tool);
        out << "== " << tool << ": " << (program.isEmpty() ? QStringLiteral("NOT FOUND") : program) << "\n";
        if (program.isEmpty())
            continue;
        const bool isFfmpeg = tool == QLatin1String("ffmpeg");
        const QString help = runTool(program, isFfmpeg ? QStringList{QStringLiteral("-hide_banner"),
                                                                     QStringLiteral("-h"), QStringLiteral("long")}
                                                       : QStringList{QStringLiteral("--help")}, 60000);
        QStringList unparsed;
        const QList<OptionSection> sections = isFfmpeg ? parseFfmpegHelp(help, &unparsed)
                                              : tool == QLatin1String("yt-dlp") ? parseYtDlpHelp(help, &unparsed)
                                              : parseArgparseHelp(help, &unparsed);
        int total = 0;
        for (const OptionSection &section : sections) {
            out << "  " << section.title << ": " << section.options.size() << "\n";
            total += section.options.size();
            for (const OptionDef &def : section.options) {
                out << "      " << def.args.join(QLatin1Char(' '));
                if (def.streamSpec)
                    out << " [spec]";
                if (def.hasValue)
                    out << " <" << def.argName << ">" << (def.valueOptional ? "?" : "") << (def.multiValue ? "*" : "");
                out << "  | " << def.description.left(50) << "\n";
            }
        }
        out << "  total options: " << total << "\n";
        for (const QString &line : std::as_const(unparsed))
            out << "  UNPARSED: " << line << "\n";

        if (isFfmpeg) {
            const auto count = [&](const QString &arg, int column, bool separator, int header, int flagWidth) {
                const auto list = parseFfmpegList(runTool(program, {QStringLiteral("-hide_banner"), arg}),
                                                  column, separator, header, flagWidth);
                out << "  " << arg << ": " << list.size();
                if (!list.isEmpty())
                    out << "  first=" << list.first().name << " [" << list.first().flags << "]  last="
                        << list.last().name << " [" << list.last().flags << "]";
                out << "\n";
            };
            count(QStringLiteral("-encoders"), 1, true, 0, 0);
            count(QStringLiteral("-formats"), 0, true, 0, 4);
            count(QStringLiteral("-pix_fmts"), 1, true, 0, 0);
            count(QStringLiteral("-filters"), 1, true, 0, 0);
            count(QStringLiteral("-sample_fmts"), 0, false, 1, 0);
            count(QStringLiteral("-hwaccels"), 0, false, 1, 0);
            count(QStringLiteral("-bsfs"), 0, false, 1, 0);
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("ytmlp"));
    QCoreApplication::setApplicationName(QStringLiteral("ytmlp"));

    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--selftest") == 0) {
            QCoreApplication app(argc, argv);
            return selfTest();
        }
    }

    QApplication app(argc, argv);

    QTabWidget window;
    window.setWindowTitle(QStringLiteral("ytmlp"));
    const QList<ToolTab *> tabs = {
        new ToolTab(ToolTab::YtDlp),   new ToolTab(ToolTab::Ffmpeg), new ToolTab(ToolTab::Spotify),
        new ToolTab(ToolTab::Instagram), new ToolTab(ToolTab::TikTok),
    };
    const QStringList titles = {QStringLiteral("yt-dlp"), QStringLiteral("ffmpeg"), QStringLiteral("Spotify"),
                                QStringLiteral("Instagram"), QStringLiteral("TikTok")};
    for (int i = 0; i < tabs.size(); ++i)
        window.addTab(tabs.at(i), titles.at(i));
    window.resize(980, 780);
    window.show();

    // `ytmlp --screenshot out.png [tab] [filter]`: saves a picture of the window and quits.
    const QStringList args = app.arguments();
    const qsizetype shot = args.indexOf(QStringLiteral("--screenshot"));
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString file = args.at(shot + 1);
        const int tab = args.value(shot + 2).toInt();
        const QString filter = args.value(shot + 3);
        QTimer::singleShot(12000, &window, [&window, tabs, file, tab, filter] {
            window.setCurrentIndex(tab);
            tabs.value(tab, tabs.first())->setFilter(filter);
            QTimer::singleShot(500, &window, [&window, file] {
                window.grab().save(file);
                QCoreApplication::quit();
            });
        });
    }

    return app.exec();
}
