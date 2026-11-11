#include "simple.h"
#include "tools.h"
#include "tooltab.h"
#include "widgets.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

// `DefDownloader --selftest`: parses the help of the installed tools and prints what
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

// The simple screen by default; the switch in the top right corner shows the
// full option tabs of every tool instead.
class MainWindow : public QWidget
{
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("DefDownloader"));
        m_simple = new SimplePage;
        m_tabBar = new QTabBar;
        m_tabBar->setExpanding(false);
        m_tabBar->setDrawBase(false);
        m_tabBar->hide();
        m_toggle = new ToggleSwitch;
        auto *toggleLabel = new QLabel(tr("Avanzata"));
        const QString tip = tr("Tutte le opzioni di yt-dlp, ffmpeg, spotDL e instaloader");
        m_toggle->setToolTip(tip);
        toggleLabel->setToolTip(tip);

        auto *header = new QHBoxLayout;
        header->setContentsMargins(12, 8, 14, 2);
        header->addWidget(m_simple->serviceBar());
        header->addWidget(m_tabBar);
        header->addStretch(1);
        header->addWidget(toggleLabel);
        header->addSpacing(4);
        header->addWidget(m_toggle);

        m_advanced = new QStackedWidget;
        m_pages = new QStackedWidget;
        m_pages->addWidget(m_simple);
        m_pages->addWidget(m_advanced);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        root->addLayout(header);
        root->addWidget(m_pages, 1);

        connect(m_tabBar, &QTabBar::currentChanged, m_advanced, &QStackedWidget::setCurrentIndex);
        connect(m_toggle, &ToggleSwitch::toggled, this, [this](bool on) {
            showAdvanced(on);
            if (m_remember)
                QSettings().setValue(QStringLiteral("ui/advanced"), on);
        });
        if (QSettings().value(QStringLiteral("ui/advanced"), false).toBool())
            m_toggle->setChecked(true);
    }

    // Used by --screenshot; does not change the saved preference.
    ToolTab *openTab(int index)
    {
        m_remember = false;
        m_toggle->setChecked(true);
        m_tabBar->setCurrentIndex(qBound(0, index, m_tabs.size() - 1));
        return m_tabs.at(m_tabBar->currentIndex());
    }

private:
    void showAdvanced(bool on)
    {
        if (on && m_tabs.isEmpty()) {
            // Built on first use: each tab runs its tool to read the options.
            const QList<QPair<ToolTab::Kind, QString>> tabs = {
                {ToolTab::YtDlp, QStringLiteral("yt-dlp")},     {ToolTab::Ffmpeg, QStringLiteral("ffmpeg")},
                {ToolTab::Spotify, QStringLiteral("Spotify")},  {ToolTab::Instagram, QStringLiteral("Instagram")},
                {ToolTab::TikTok, QStringLiteral("TikTok")},
            };
            for (const auto &[kind, title] : tabs) {
                m_tabs.append(new ToolTab(kind));
                m_advanced->addWidget(m_tabs.last());
                m_tabBar->addTab(title);
            }
        }
        m_simple->serviceBar()->setVisible(!on);
        m_tabBar->setVisible(on);
        m_pages->setCurrentIndex(on ? 1 : 0);
    }

    SimplePage *m_simple;
    QTabBar *m_tabBar;
    ToggleSwitch *m_toggle;
    QStackedWidget *m_pages;
    QStackedWidget *m_advanced;
    QList<ToolTab *> m_tabs;
    bool m_remember = true;
};

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("DefDownloader"));
    QCoreApplication::setApplicationName(QStringLiteral("DefDownloader"));

    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--selftest") == 0) {
            QCoreApplication app(argc, argv);
            return selfTest();
        }
    }

    QApplication app(argc, argv);

    MainWindow window;
    window.resize(980, 780);
    window.show();

    // `DefDownloader --screenshot out.png [tab] [filter]`: saves a picture of the window and quits.
    // Without a tab number the picture shows the simple screen.
    const QStringList args = app.arguments();
    const qsizetype shot = args.indexOf(QStringLiteral("--screenshot"));
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString file = args.at(shot + 1);
        bool hasTab = false;
        const int tab = args.value(shot + 2).toInt(&hasTab);
        const QString filter = args.value(shot + 3);
        ToolTab *page = hasTab ? window.openTab(tab) : nullptr;
        // The tabs need time to run their tool and read its options.
        QTimer::singleShot(hasTab ? 12000 : 1000, &window, [&window, page, file, filter] {
            if (page)
                page->setFilter(filter);
            QTimer::singleShot(500, &window, [&window, file] {
                window.grab().save(file);
                QCoreApplication::quit();
            });
        });
    }

    return app.exec();
}
