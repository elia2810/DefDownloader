#include "convert.h"
#include "simple.h"
#include "tools.h"
#include "tooltab.h"
#include "widgets.h"

#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <functional>
#include <memory>

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

// "it" or "en": what the user picked, otherwise the language of the system
// (Italian only on an Italian system). `--lang` wins over both, without being saved.
static QString g_languageOverride;
static QString currentLanguage()
{
    QString language = g_languageOverride;
    if (language.isEmpty())
        language = QSettings().value(QStringLiteral("ui/language")).toString();
    if (language == QLatin1String("it") || language == QLatin1String("en"))
        return language;
    const QStringList system = QLocale::system().uiLanguages();
    return !system.isEmpty() && system.first().startsWith(QLatin1String("it"), Qt::CaseInsensitive)
               ? QStringLiteral("it") : QStringLiteral("en");
}

// The simple screen by default, with the ready-made conversions (FFMPEG) one
// button away; the switch in the top right corner shows the full option tabs
// of every tool instead.
class MainWindow : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(MainWindow)

public:
    // Called with "it" or "en" when another language is picked: the texts are
    // set when the widgets are built, so the window has to be built again.
    std::function<void(const QString &)> languageChosen;

    MainWindow()
    {
        setWindowTitle(QStringLiteral("DefDownloader"));
        m_simple = new SimplePage;
        m_convert = new ConvertPage;
        // Part of the same group as the site buttons: one screen at a time.
        m_convertButton = headerButton(ffmpegIcon(), QStringLiteral("FFMPEG"));
        m_convertButton->setToolTip(tr("Converti, comprimi e migliora un file video o audio"));
        m_simple->serviceGroup()->addButton(m_convertButton, kConvertId);
        m_tabBar = new QTabBar;
        m_tabBar->setExpanding(false);
        m_tabBar->setDrawBase(false);
        m_tabBar->hide();
        m_toggle = new ToggleSwitch;
        auto *toggleLabel = new QLabel(tr("Avanzata"));
        const QString tip = tr("Tutte le opzioni di yt-dlp, ffmpeg, spotDL e instaloader");
        m_toggle->setToolTip(tip);
        toggleLabel->setToolTip(tip);

        // --- language ----------------------------------------------------------
        auto *languageButton = new QToolButton;
        languageButton->setText(currentLanguage().toUpper());
        languageButton->setToolTip(tr("Lingua"));
        languageButton->setPopupMode(QToolButton::InstantPopup);
        languageButton->setCursor(Qt::PointingHandCursor);
        languageButton->setStyleSheet(QStringLiteral(
            "QToolButton { border: 1px solid rgba(128, 128, 128, 0.45); border-radius: 6px; padding: 2px 7px; }"
            "QToolButton:hover { background: rgba(128, 128, 128, 0.2); }"
            "QToolButton::menu-indicator { image: none; }"));
        auto *languageMenu = new QMenu(languageButton);
        auto *languages = new QActionGroup(languageMenu);
        const QList<QPair<QString, QString>> names = {{QStringLiteral("it"), QStringLiteral("Italiano")},
                                                      {QStringLiteral("en"), QStringLiteral("English")}};
        for (const auto &[code, name] : names) {
            QAction *action = languageMenu->addAction(name);
            action->setCheckable(true);
            action->setChecked(code == currentLanguage());
            languages->addAction(action);
            connect(action, &QAction::triggered, this, [this, code = code] { chooseLanguage(code); });
        }
        languageButton->setMenu(languageMenu);

        auto *header = new QHBoxLayout;
        header->setContentsMargins(12, 8, 14, 2);
        header->addWidget(m_simple->serviceBar());
        header->addWidget(m_tabBar);
        header->addStretch(1);
        header->addWidget(m_convertButton);
        header->addSpacing(14);
        header->addWidget(toggleLabel);
        header->addSpacing(4);
        header->addWidget(m_toggle);
        header->addSpacing(12);
        header->addWidget(languageButton);

        // Shown when GitHub has a newer release than this copy of the app.
        auto *updateFrame = new QFrame;
        updateFrame->setObjectName(QStringLiteral("updateBar"));
        updateFrame->setStyleSheet(QStringLiteral(
            "#updateBar { background: rgba(255, 170, 30, 0.20); border: 1px solid rgba(255, 170, 30, 0.65);"
            " border-radius: 8px; }"));
        m_updateLabel = new QLabel;
        auto *getUpdate = new QPushButton(tr("Scarica"));
        auto *later = new QPushButton(tr("Non ora"));
        getUpdate->setAutoDefault(false);
        later->setAutoDefault(false);
        auto *updateLayout = new QHBoxLayout(updateFrame);
        updateLayout->setContentsMargins(12, 6, 8, 6);
        updateLayout->addWidget(m_updateLabel, 1);
        updateLayout->addWidget(getUpdate);
        updateLayout->addWidget(later);
        m_updateBar = new QWidget; // frame and margins appear and disappear together
        auto *updateRow = new QHBoxLayout(m_updateBar);
        updateRow->setContentsMargins(12, 4, 12, 4);
        updateRow->addWidget(updateFrame);
        m_updateBar->hide();
        connect(getUpdate, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kReleasesUrl)); });
        connect(later, &QPushButton::clicked, m_updateBar, &QWidget::hide);

        m_advanced = new QStackedWidget;
        m_pages = new QStackedWidget;
        m_pages->addWidget(m_simple);
        m_pages->addWidget(m_advanced);
        m_pages->addWidget(m_convert);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        root->addLayout(header);
        root->addWidget(m_updateBar);
        root->addWidget(m_pages, 1);

        connect(m_tabBar, &QTabBar::currentChanged, m_advanced, &QStackedWidget::setCurrentIndex);
        connect(m_simple->serviceGroup(), &QButtonGroup::idClicked, this, [this] { showAdvanced(false); });
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

    // Asked once, when the app opens. Without an answer nothing is shown.
    void checkForUpdate()
    {
        fetchLatestVersion(this, [this](const QString &latest) {
            if (QVersionNumber::fromString(latest) > QVersionNumber::fromString(QCoreApplication::applicationVersion()))
                showUpdate(latest);
        });
    }

    void showUpdate(const QString &latest)
    {
        m_updateLabel->setText(tr("È uscita la versione %1 di DefDownloader (questa è la %2).")
                                   .arg(latest, QCoreApplication::applicationVersion()));
        m_updateBar->show();
    }

    // Used by --screenshot: the conversion screen, with a file when one is given.
    void openConvert(const QString &file)
    {
        m_remember = false;
        m_toggle->setChecked(false);
        m_convertButton->click();
        if (!file.isEmpty())
            m_convert->setInput(file);
    }

private:
    enum { kConvertId = 100 }; // id of the FFMPEG button among the site buttons

    void chooseLanguage(const QString &code)
    {
        if (code == currentLanguage() || !languageChosen)
            return;
        // The new window starts empty: say so when something would be lost.
        bool busy = !findChildren<DownloadRow *>().isEmpty();
        const QList<QProcess *> processes = m_convert->findChildren<QProcess *>();
        for (const QProcess *process : processes)
            busy = busy || process->state() != QProcess::NotRunning;
        if (busy
            && QMessageBox::question(this, tr("Lingua"),
                                     tr("Per cambiare lingua la finestra viene ricreata: la lista si svuota e "
                                        "quello che è in corso viene fermato. Continuare?"))
                   != QMessageBox::Yes)
            return;
        languageChosen(code);
    }

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
        m_convertButton->setVisible(!on);
        m_tabBar->setVisible(on);
        m_pages->setCurrentWidget(on ? static_cast<QWidget *>(m_advanced)
                                  : m_convertButton->isChecked() ? static_cast<QWidget *>(m_convert)
                                                                 : static_cast<QWidget *>(m_simple));
    }

    SimplePage *m_simple;
    ConvertPage *m_convert;
    QToolButton *m_convertButton;
    QWidget *m_updateBar;
    QLabel *m_updateLabel;
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
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));

    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--selftest") == 0) {
            QCoreApplication app(argc, argv);
            return selfTest();
        }
    }

    QApplication app(argc, argv);
#ifdef Q_OS_LINUX
    // The name of the installed .desktop file: how a Wayland desktop matches the
    // window with its menu entry. The picture covers X11 and an app that is not
    // installed (the AppImage).
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.elia2810.DefDownloader"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/DefDownloader.png")));
#endif
    const QStringList args = app.arguments();
    const qsizetype shot = args.indexOf(QStringLiteral("--screenshot"));
    // `--lang it|en`: the language for this run only.
    const qsizetype lang = args.indexOf(QStringLiteral("--lang"));
    if (lang >= 0)
        g_languageOverride = args.value(lang + 1);

    // The texts in the code are Italian; English is a translation file inside the app.
    QTranslator translator;
    const auto applyLanguage = [&translator] {
        QCoreApplication::removeTranslator(&translator);
        if (currentLanguage() == QLatin1String("en") && translator.load(QStringLiteral(":/i18n/DefDownloader_en.qm")))
            QCoreApplication::installTranslator(&translator);
    };
    applyLanguage();

    std::unique_ptr<MainWindow> window;
    std::function<void()> openWindow = [&] {
        const QRect geometry = window ? window->geometry() : QRect();
        window.reset(); // what the old window was running stops with it
        window = std::make_unique<MainWindow>();
        window->languageChosen = [&](const QString &code) {
            g_languageOverride.clear();
            QSettings().setValue(QStringLiteral("ui/language"), code);
            // Not from inside the menu of the window that is going away.
            QTimer::singleShot(0, qApp, [&] {
                applyLanguage();
                openWindow();
            });
        };
        if (geometry.isValid())
            window->setGeometry(geometry);
        else
            window->resize(980, 780);
        window->show();
        if (shot < 0)
            window->checkForUpdate(); // pictures stay the same whatever is published
    };
    openWindow();

    // `DefDownloader --screenshot out.png [tab] [filter]`: saves a picture of the window and quits.
    // Without a tab number the picture shows the simple screen; `ffmpeg [file]`
    // instead of the number shows the conversion screen.
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString file = args.at(shot + 1);
        bool hasTab = false;
        const int tab = args.value(shot + 2).toInt(&hasTab);
        const QString filter = args.value(shot + 3);
        MainWindow *shown = window.get();
        ToolTab *page = hasTab ? shown->openTab(tab) : nullptr;
        if (args.value(shot + 2) == QLatin1String("ffmpeg"))
            shown->openConvert(filter);
        // The tabs need time to run their tool and read its options.
        QTimer::singleShot(hasTab ? 12000 : 1000, shown, [shown, page, file, filter] {
            if (page)
                page->setFilter(filter);
            QTimer::singleShot(500, shown, [shown, file] {
                shown->grab().save(file);
                QCoreApplication::quit();
            });
        });
    }

    return app.exec();
}
