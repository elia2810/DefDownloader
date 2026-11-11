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
    for (const QString &tool : {QStringLiteral("yt-dlp"), QStringLiteral("ffmpeg")}) {
        const QString program = findTool(tool);
        out << "== " << tool << ": " << (program.isEmpty() ? QStringLiteral("NOT FOUND") : program) << "\n";
        if (program.isEmpty())
            continue;
        const bool isYtDlp = tool == QLatin1String("yt-dlp");
        const QString help = runTool(program, isYtDlp ? QStringList{QStringLiteral("--help")}
                                                      : QStringList{QStringLiteral("-hide_banner"),
                                                                    QStringLiteral("-h"), QStringLiteral("long")});
        QStringList unparsed;
        const QList<OptionSection> sections = isYtDlp ? parseYtDlpHelp(help, &unparsed)
                                                      : parseFfmpegHelp(help, &unparsed);
        int total = 0;
        for (const OptionSection &section : sections) {
            out << "  " << section.title << ": " << section.options.size() << "\n";
            total += section.options.size();
            for (const OptionDef &def : section.options) {
                out << "      " << def.args.join(QLatin1Char(' '));
                if (def.streamSpec)
                    out << " [spec]";
                if (def.hasValue)
                    out << " <" << def.argName << ">";
                out << "  | " << def.description.left(50) << "\n";
            }
        }
        out << "  total options: " << total << "\n";
        for (const QString &line : std::as_const(unparsed))
            out << "  UNPARSED: " << line << "\n";

        if (!isYtDlp) {
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
    auto *ytDlp = new ToolTab(ToolTab::YtDlp);
    auto *ffmpeg = new ToolTab(ToolTab::Ffmpeg);
    window.addTab(ytDlp, QStringLiteral("yt-dlp"));
    window.addTab(ffmpeg, QStringLiteral("ffmpeg"));
    window.resize(980, 780);
    window.show();

    // `ytmlp --screenshot out.png [tab] [filter]`: saves a picture of the window and quits.
    const QStringList args = app.arguments();
    const qsizetype shot = args.indexOf(QStringLiteral("--screenshot"));
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString file = args.at(shot + 1);
        const int tab = args.value(shot + 2).toInt();
        const QString filter = args.value(shot + 3);
        QTimer::singleShot(4000, &window, [&window, ytDlp, ffmpeg, file, tab, filter] {
            window.setCurrentIndex(tab);
            (tab == 0 ? ytDlp : ffmpeg)->setFilter(filter);
            QTimer::singleShot(500, &window, [&window, file] {
                window.grab().save(file);
                QCoreApplication::quit();
            });
        });
    }

    return app.exec();
}
