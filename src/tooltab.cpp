#include "tooltab.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Which ffmpeg capability list can fill in the value of an option.
struct Picker {
    QString listArg;
    int nameColumn = 0;
    bool hasSeparator = false;
    int headerLines = 0;
    int flagWidth = 0;
    QChar typeFilter;      // encoders: V / A / S
    bool append = false;   // comma-separated chains (filters)
    QStringList extra;     // values that are not in the list
    QString title;
};

bool pickerFor(const OptionDef &def, Picker &p)
{
    const QString name = def.label.mid(1);
    const QString arg = def.argName;

    if (arg == QLatin1String("codec")) {
        p = {QStringLiteral("-encoders"), 1, true, 0, 0, {}, false, {QStringLiteral("copy")},
             QStringLiteral("Codec")};
        if (name == QLatin1String("vcodec"))
            p.typeFilter = QLatin1Char('V');
        else if (name == QLatin1String("acodec"))
            p.typeFilter = QLatin1Char('A');
        else if (name == QLatin1String("scodec"))
            p.typeFilter = QLatin1Char('S');
        return true;
    }
    if (name == QLatin1String("f")) {
        p = {QStringLiteral("-formats"), 0, true, 0, 4, {}, false, {}, QStringLiteral("Formato")};
        return true;
    }
    if (name == QLatin1String("pix_fmt")) {
        p = {QStringLiteral("-pix_fmts"), 1, true, 0, 0, {}, false, {}, QStringLiteral("Pixel format")};
        return true;
    }
    if (name == QLatin1String("sample_fmt")) {
        p = {QStringLiteral("-sample_fmts"), 0, false, 1, 0, {}, false, {},
             QStringLiteral("Sample format")};
        return true;
    }
    if (name == QLatin1String("hwaccel")) {
        p = {QStringLiteral("-hwaccels"), 0, false, 1, 0, {}, false, {},
             QStringLiteral("Accelerazione hardware")};
        return true;
    }
    if (arg == QLatin1String("filter_graph") || arg == QLatin1String("graph_description")
        || name == QLatin1String("filter") || name == QLatin1String("lavfi")) {
        p = {QStringLiteral("-filters"), 1, true, 0, 0, {}, true, {}, QStringLiteral("Filtro")};
        return true;
    }
    if (name == QLatin1String("bsf") || arg == QLatin1String("bitstream_filters")) {
        p = {QStringLiteral("-bsfs"), 0, false, 1, 0, {}, true, {}, QStringLiteral("Bitstream filter")};
        return true;
    }
    return false;
}

// Searchable list dialog; returns the chosen name or an empty string.
QString pickFromList(QWidget *parent, const QString &title, const QList<ListEntry> &entries)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(560, 480);

    auto *search = new QLineEdit;
    search->setPlaceholderText(QObject::tr("Cerca…"));
    search->setClearButtonEnabled(true);
    auto *list = new QListWidget;
    for (const ListEntry &entry : entries) {
        QString text = entry.name;
        if (!entry.description.isEmpty())
            text += QStringLiteral("  —  ") + entry.description;
        auto *item = new QListWidgetItem(text, list);
        item->setData(Qt::UserRole, entry.name);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(search);
    layout->addWidget(list, 1);
    layout->addWidget(buttons);

    QObject::connect(search, &QLineEdit::textChanged, list, [list](const QString &text) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted || !list->currentItem() || list->currentItem()->isHidden())
        return {};
    return list->currentItem()->data(Qt::UserRole).toString();
}

QPushButton *smallButton(const QString &text)
{
    auto *button = new QPushButton(text);
    button->setAutoDefault(false);
    return button;
}

QString downloadsDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return dir.isEmpty() ? QDir::homePath() : dir;
}

} // namespace

ToolTab::ToolTab(Kind kind, QWidget *parent)
    : QWidget(parent)
    , m_kind(kind)
    , m_toolName(kind == YtDlp ? QStringLiteral("yt-dlp") : QStringLiteral("ffmpeg"))
{
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    auto *root = new QVBoxLayout(this);

    // --- what to work on ---------------------------------------------------
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto withButton = [](QLineEdit *edit, QPushButton *button) {
        auto *row = new QHBoxLayout;
        row->addWidget(edit, 1);
        row->addWidget(button);
        return row;
    };

    if (m_kind == YtDlp) {
        m_urlEdit = new QLineEdit;
        m_urlEdit->setPlaceholderText(tr("Uno o più URL separati da spazio"));
        m_urlEdit->setClearButtonEnabled(true);
        form->addRow(tr("URL"), m_urlEdit);

        m_dirEdit = new QLineEdit(QSettings().value(QStringLiteral("ytdlp/dir"), downloadsDir()).toString());
        auto *browse = smallButton(tr("Sfoglia…"));
        form->addRow(tr("Cartella"), withButton(m_dirEdit, browse));
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Cartella di destinazione"),
                                                                  m_dirEdit->text());
            if (!dir.isEmpty()) {
                m_dirEdit->setText(dir);
                QSettings().setValue(QStringLiteral("ytdlp/dir"), dir);
            }
        });
        connect(m_dirEdit, &QLineEdit::editingFinished, this,
                [this] { QSettings().setValue(QStringLiteral("ytdlp/dir"), m_dirEdit->text()); });
        connect(m_urlEdit, &QLineEdit::textChanged, this, &ToolTab::updatePreview);
    } else {
        m_inputEdit = new QLineEdit;
        m_inputEdit->setPlaceholderText(tr("File di input (più file separati da ;)"));
        m_inputEdit->setClearButtonEnabled(true);
        auto *browseIn = smallButton(tr("Sfoglia…"));
        form->addRow(tr("Input"), withButton(m_inputEdit, browseIn));
        connect(browseIn, &QPushButton::clicked, this, [this] {
            const QStringList files = QFileDialog::getOpenFileNames(this, tr("File di input"));
            if (!files.isEmpty())
                m_inputEdit->setText(files.join(QStringLiteral("; ")));
        });

        m_outputEdit = new QLineEdit;
        m_outputEdit->setPlaceholderText(tr("File di output (un nome senza percorso finisce in Download)"));
        m_outputEdit->setClearButtonEnabled(true);
        auto *browseOut = smallButton(tr("Sfoglia…"));
        form->addRow(tr("Output"), withButton(m_outputEdit, browseOut));
        connect(browseOut, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getSaveFileName(this, tr("File di output"), workingDirectory());
            if (!file.isEmpty())
                m_outputEdit->setText(file);
        });
        connect(m_inputEdit, &QLineEdit::textChanged, this, &ToolTab::updatePreview);
        connect(m_outputEdit, &QLineEdit::textChanged, this, &ToolTab::updatePreview);
    }
    root->addLayout(form);

    // --- options -----------------------------------------------------------
    auto *filterRow = new QHBoxLayout;
    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText(tr("Cerca tra le opzioni…"));
    m_filterEdit->setClearButtonEnabled(true);
    auto *resetButton = smallButton(tr("Azzera opzioni"));
    filterRow->addWidget(m_filterEdit, 1);
    filterRow->addWidget(resetButton);
    root->addLayout(filterRow);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &ToolTab::applyFilter);
    connect(resetButton, &QPushButton::clicked, this, &ToolTab::resetOptions);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_optionsHost = new QWidget;
    m_optionsLayout = new QVBoxLayout(m_optionsHost);
    m_optionsLayout->setContentsMargins(4, 4, 4, 4);
    m_optionsLayout->setSpacing(0);
    scroll->setWidget(m_optionsHost);

    // --- command, run, output ---------------------------------------------
    auto *bottom = new QWidget;
    auto *bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 0, 0, 0);

    auto *extraForm = new QFormLayout;
    extraForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_extraEdit = new QLineEdit;
    m_extraEdit->setFont(mono);
    m_extraEdit->setPlaceholderText(tr("Argomenti aggiuntivi scritti a mano (facoltativi)"));
    extraForm->addRow(tr("Extra"), m_extraEdit);
    bottomLayout->addLayout(extraForm);
    connect(m_extraEdit, &QLineEdit::textChanged, this, &ToolTab::updatePreview);

    auto *commandRow = new QHBoxLayout;
    m_commandEdit = new QLineEdit;
    m_commandEdit->setReadOnly(true);
    m_commandEdit->setFont(mono);
    auto *copyButton = smallButton(tr("Copia"));
    m_runButton = smallButton(tr("Esegui"));
    m_stopButton = smallButton(tr("Ferma"));
    m_stopButton->setEnabled(false);
    commandRow->addWidget(m_commandEdit, 1);
    commandRow->addWidget(copyButton);
    commandRow->addWidget(m_runButton);
    commandRow->addWidget(m_stopButton);
    bottomLayout->addLayout(commandRow);
    connect(copyButton, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(m_commandEdit->text()); });
    connect(m_runButton, &QPushButton::clicked, this, &ToolTab::run);
    connect(m_stopButton, &QPushButton::clicked, this, &ToolTab::stop);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setFont(mono);
    m_log->setMaximumBlockCount(5000);
    m_log->setPlaceholderText(tr("L'output del comando appare qui"));
    bottomLayout->addWidget(m_log, 1);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(scroll);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);
    splitter->setSizes({460, 240});
    root->addWidget(splitter, 1);

    updatePreview();
    loadOptions();
}

ToolTab::~ToolTab()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

// ---------------------------------------------------------------------------
// Options: read from the tool's own help, so they always match the installed
// version.

void ToolTab::loadOptions()
{
    clearOptions();
    m_program = findTool(m_toolName);
    if (m_program.isEmpty()) {
        showMessage(tr("%1 non trovato. Installalo oppure indica dove si trova.").arg(m_toolName), true);
        emit optionsLoaded();
        return;
    }
    showMessage(tr("Lettura delle opzioni di %1…").arg(m_toolName), false);

    auto *process = new QProcess(this);
    process->setProcessEnvironment(toolEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);
    const auto finish = [this, process] {
        const QString help = QString::fromUtf8(process->readAll());
        process->deleteLater();
        const QList<OptionSection> sections = m_kind == YtDlp ? parseYtDlpHelp(help)
                                                              : parseFfmpegHelp(help);
        clearOptions();
        if (sections.isEmpty())
            showMessage(tr("Impossibile leggere le opzioni da %1.").arg(m_program), true);
        else
            buildOptionWidgets(sections);
        emit optionsLoaded();
    };
    connect(process, &QProcess::finished, this, finish);
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish();
    });
    process->start(m_program, m_kind == YtDlp
                                  ? QStringList{QStringLiteral("--help")}
                                  : QStringList{QStringLiteral("-hide_banner"), QStringLiteral("-h"),
                                                QStringLiteral("long")});
}

void ToolTab::clearOptions()
{
    m_rows.clear();
    m_sections.clear();
    while (QLayoutItem *item = m_optionsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void ToolTab::showMessage(const QString &text, bool offerChooser)
{
    clearOptions();
    auto *label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    m_optionsLayout->addSpacing(40);
    m_optionsLayout->addWidget(label);
    if (offerChooser) {
        auto *button = smallButton(tr("Scegli eseguibile…"));
        m_optionsLayout->addSpacing(10);
        m_optionsLayout->addWidget(button, 0, Qt::AlignHCenter);
        connect(button, &QPushButton::clicked, this, &ToolTab::chooseProgram);
    }
    m_optionsLayout->addStretch(1);
}

void ToolTab::chooseProgram()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Eseguibile di %1").arg(m_toolName));
    if (file.isEmpty())
        return;
    QSettings().setValue(QStringLiteral("paths/") + m_toolName, file);
    m_listCache.clear();
    // Deferred: this runs from a button that loadOptions() is about to delete.
    QTimer::singleShot(0, this, &ToolTab::loadOptions);
}

void ToolTab::buildOptionWidgets(const QList<OptionSection> &sections)
{
    m_optionsHost->setUpdatesEnabled(false);
    for (const OptionSection &section : sections) {
        const int sectionIndex = m_sections.size();
        Section s;
        s.title = section.title;
        s.container = new QWidget;
        auto *containerLayout = new QVBoxLayout(s.container);
        containerLayout->setContentsMargins(0, 0, 0, 0);
        containerLayout->setSpacing(0);

        s.header = new QPushButton;
        s.header->setAutoDefault(false);
        s.header->setCursor(Qt::PointingHandCursor);
        s.header->setStyleSheet(QStringLiteral(
            "QPushButton { border: none; text-align: left; padding: 7px 6px; font-weight: bold; }"));
        containerLayout->addWidget(s.header);
        connect(s.header, &QPushButton::clicked, this, [this, sectionIndex] { toggleSection(sectionIndex); });

        s.body = new QWidget;
        auto *bodyLayout = new QVBoxLayout(s.body);
        bodyLayout->setContentsMargins(0, 0, 0, 8);
        bodyLayout->setSpacing(0);
        for (const OptionDef &def : section.options) {
            const int rowIndex = m_rows.size();
            Row row;
            row.def = def;
            row.section = sectionIndex;
            m_rows.append(row);
            s.rows.append(rowIndex);
            bodyLayout->addWidget(createRow(rowIndex));
        }
        s.body->setVisible(false);
        containerLayout->addWidget(s.body);

        m_sections.append(s);
        m_optionsLayout->addWidget(s.container);
        updateSectionHeader(sectionIndex);
    }
    m_optionsLayout->addStretch(1);
    m_optionsHost->setUpdatesEnabled(true);
    applyFilter();
    updatePreview();
}

QWidget *ToolTab::createRow(int index)
{
    Row &row = m_rows[index];
    const OptionDef &def = row.def;

    row.widget = new QWidget;
    auto *outer = new QVBoxLayout(row.widget);
    outer->setContentsMargins(24, 4, 6, 4);
    outer->setSpacing(1);
    auto *line = new QHBoxLayout;
    line->setSpacing(6);

    row.check = new QCheckBox(def.label);
    row.check->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    line->addWidget(row.check);
    connect(row.check, &QCheckBox::toggled, this, [this, index] { optionChanged(index); });

    if (def.streamSpec) {
        row.spec = new QLineEdit;
        row.spec->setPlaceholderText(tr("stream: v, a, s:0…"));
        row.spec->setFixedWidth(120);
        line->addWidget(row.spec);
        connect(row.spec, &QLineEdit::textChanged, this, [this, index] { optionChanged(index); });
    }

    if (def.hasValue) {
        row.value = new QLineEdit;
        row.value->setPlaceholderText(def.argName.isEmpty() ? tr("valore, se richiesto") : def.argName);
        if (def.argName.isEmpty())
            row.value->setFixedWidth(170);
        line->addWidget(row.value, def.argName.isEmpty() ? 0 : 1);
        connect(row.value, &QLineEdit::textChanged, this, [this, index](const QString &text) {
            if (!text.isEmpty())
                m_rows[index].check->setChecked(true);
            optionChanged(index);
        });
        addValueHelpers(row, line);
    }

    if (def.position == OptionDef::Either) {
        row.where = new QComboBox;
        row.where->addItems({tr("output"), tr("input")});
        row.where->setCurrentIndex(def.defaultInput ? 1 : 0);
        line->addWidget(row.where);
        connect(row.where, &QComboBox::currentIndexChanged, this, [this, index] { optionChanged(index); });
    }
    if (!def.hasValue || def.argName.isEmpty())
        line->addStretch(1);
    outer->addLayout(line);

    if (!def.description.isEmpty()) {
        auto *description = new QLabel(def.description);
        description->setTextFormat(Qt::PlainText); // descriptions contain <placeholders>
        description->setWordWrap(true);
        QFont small = description->font();
        small.setPointSizeF(small.pointSizeF() * 0.9);
        description->setFont(small);
        QPalette palette = description->palette();
        palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
        description->setPalette(palette);
        description->setContentsMargins(22, 0, 0, 0);
        outer->addWidget(description);
    }
    return row.widget;
}

// Browse buttons for paths and, for ffmpeg, pickers fed by its capability lists.
void ToolTab::addValueHelpers(Row &row, QHBoxLayout *layout)
{
    const QString arg = row.def.argName;
    QLineEdit *value = row.value;

    const bool wantsFile = m_kind == YtDlp ? arg.contains(QLatin1String("FILE"))
                                           : (arg == QLatin1String("filename") || arg == QLatin1String("file"));
    const bool wantsDir = m_kind == YtDlp && (arg == QLatin1String("DIR") || arg.endsWith(QLatin1String("PATH")));
    if (wantsFile || wantsDir) {
        auto *browse = smallButton(tr("Sfoglia…"));
        layout->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, value, wantsDir] {
            const QString path = wantsDir ? QFileDialog::getExistingDirectory(this, {}, value->text())
                                          : QFileDialog::getOpenFileName(this, {}, value->text());
            if (!path.isEmpty())
                value->setText(path);
        });
        return;
    }

    Picker picker;
    if (m_kind != Ffmpeg || !pickerFor(row.def, picker))
        return;
    auto *choose = smallButton(tr("Scegli…"));
    layout->addWidget(choose);
    connect(choose, &QPushButton::clicked, this, [this, value, picker] {
        QList<ListEntry> entries;
        for (const QString &name : picker.extra)
            entries.append(ListEntry{name, {}, {}});
        for (const ListEntry &entry : capabilityList(picker.listArg, picker.nameColumn, picker.hasSeparator,
                                                      picker.headerLines, picker.flagWidth)) {
            if (picker.typeFilter.isNull() || entry.flags.startsWith(picker.typeFilter))
                entries.append(entry);
        }
        const QString chosen = pickFromList(this, picker.title, entries);
        if (chosen.isEmpty())
            return;
        if (picker.append && !value->text().isEmpty())
            value->setText(value->text() + QLatin1Char(',') + chosen);
        else
            value->setText(chosen);
    });
}

QList<ListEntry> ToolTab::capabilityList(const QString &listArg, int nameColumn, bool hasSeparator,
                                         int headerLines, int flagWidth)
{
    auto it = m_listCache.constFind(listArg);
    if (it == m_listCache.constEnd()) {
        const QString text = runTool(m_program, {QStringLiteral("-hide_banner"), listArg});
        it = m_listCache.insert(listArg, parseFfmpegList(text, nameColumn, hasSeparator, headerLines, flagWidth));
    }
    return *it;
}

void ToolTab::setFilter(const QString &text)
{
    m_filterEdit->setText(text);
}

void ToolTab::applyFilter()
{
    const QString needle = m_filterEdit->text().trimmed();
    m_optionsHost->setUpdatesEnabled(false);
    for (Section &section : m_sections) {
        int visible = 0;
        for (int index : std::as_const(section.rows)) {
            const Row &row = m_rows.at(index);
            const bool match = needle.isEmpty() || row.def.label.contains(needle, Qt::CaseInsensitive)
                               || row.def.argName.contains(needle, Qt::CaseInsensitive)
                               || row.def.description.contains(needle, Qt::CaseInsensitive);
            row.widget->setVisible(match);
            visible += match;
        }
        const bool open = needle.isEmpty() ? section.expanded : true;
        section.container->setVisible(visible > 0);
        section.body->setVisible(open);
    }
    for (int i = 0; i < m_sections.size(); ++i)
        updateSectionHeader(i);
    m_optionsHost->setUpdatesEnabled(true);
}

void ToolTab::toggleSection(int index)
{
    Section &section = m_sections[index];
    const bool open = section.body->isHidden();
    section.body->setVisible(open);
    if (m_filterEdit->text().trimmed().isEmpty())
        section.expanded = open;
    updateSectionHeader(index);
}

void ToolTab::updateSectionHeader(int index)
{
    const Section &section = m_sections.at(index);
    int active = 0;
    for (int row : section.rows)
        active += m_rows.at(row).check->isChecked();
    QString text = QStringLiteral("%1  %2  (%3)")
                       .arg(section.body->isHidden() ? QStringLiteral("▸") : QStringLiteral("▾"), section.title)
                       .arg(section.rows.size());
    if (active == 1)
        text += tr("  ·  1 attiva");
    else if (active > 1)
        text += tr("  ·  %1 attive").arg(active);
    section.header->setText(text);
}

void ToolTab::optionChanged(int rowIndex)
{
    updateSectionHeader(m_rows.at(rowIndex).section);
    updatePreview();
}

void ToolTab::resetOptions()
{
    for (Row &row : m_rows) {
        if (row.value)
            row.value->clear();
        if (row.spec)
            row.spec->clear();
        if (row.where)
            row.where->setCurrentIndex(row.def.defaultInput ? 1 : 0);
        row.check->setChecked(false);
    }
    m_extraEdit->clear();
}

// ---------------------------------------------------------------------------
// Command line

QStringList ToolTab::buildArguments() const
{
    QStringList global, input, output;
    for (const Row &row : m_rows) {
        if (!row.check || !row.check->isChecked())
            continue;
        QStringList args = row.def.args;
        if (row.spec) {
            QString spec = row.spec->text().trimmed();
            while (spec.startsWith(QLatin1Char(':')))
                spec.remove(0, 1);
            if (!spec.isEmpty())
                args.last() += QLatin1Char(':') + spec;
        }
        if (row.value && (!row.value->text().isEmpty() || !row.def.valueOptional))
            args.append(row.value->text());

        OptionDef::Position position = row.def.position;
        if (position == OptionDef::Either)
            position = row.where && row.where->currentIndex() == 1 ? OptionDef::Input : OptionDef::Output;
        (position == OptionDef::Global ? global : position == OptionDef::Input ? input : output) += args;
    }
    const QStringList extra = QProcess::splitCommand(m_extraEdit->text());

    if (m_kind == YtDlp)
        return global + extra + m_urlEdit->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);

    // ffmpeg [global] {[input options] -i input}... [output options] output
    QStringList result = global;
    const QStringList inputs = m_inputEdit->text().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    bool anyInput = false;
    for (const QString &file : inputs) {
        if (file.trimmed().isEmpty())
            continue;
        result += input;
        result << QStringLiteral("-i") << file.trimmed();
        anyInput = true;
    }
    if (!anyInput)
        result += input;
    result += output + extra;
    if (!m_outputEdit->text().trimmed().isEmpty())
        result << m_outputEdit->text().trimmed();
    return result;
}

void ToolTab::updatePreview()
{
    QStringList parts{m_toolName};
    for (const QString &arg : buildArguments())
        parts << shellQuote(arg);
    m_commandEdit->setText(parts.join(QLatin1Char(' ')));
    m_commandEdit->setCursorPosition(0);
}

QString ToolTab::workingDirectory() const
{
    if (m_dirEdit && QFileInfo(m_dirEdit->text()).isDir())
        return m_dirEdit->text();
    return downloadsDir();
}

// ---------------------------------------------------------------------------
// Running

void ToolTab::run()
{
    if (m_process)
        return;
    if (m_program.isEmpty()) {
        appendOutput(tr("%1 non trovato.\n").arg(m_toolName));
        return;
    }
    if (m_dirEdit && !QFileInfo(m_dirEdit->text()).isDir()) {
        appendOutput(tr("La cartella \"%1\" non esiste.\n").arg(m_dirEdit->text()));
        return;
    }

    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    appendOutput(QStringLiteral("$ ") + m_commandEdit->text() + QLatin1Char('\n'));

    m_process = new QProcess(this);
    m_process->setProcessEnvironment(toolEnvironment());
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setWorkingDirectory(workingDirectory());
    connect(m_process, &QProcess::readyRead, this,
            [this] { appendOutput(m_decoder.decode(m_process->readAll())); });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        appendOutput(m_decoder.decode(m_process->readAll()));
        processDone(status == QProcess::CrashExit ? tr("interrotto") : tr("terminato con codice %1").arg(code));
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            processDone(tr("impossibile avviare %1").arg(m_program));
    });

    m_runButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_process->start(m_program, buildArguments());
    m_process->closeWriteChannel(); // no interactive prompts: ffmpeg must not wait on stdin
}

void ToolTab::stop()
{
    if (!m_process)
        return;
    QProcess *process = m_process;
    process->terminate(); // lets ffmpeg finalize the output file
    QTimer::singleShot(3000, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void ToolTab::processDone(const QString &message)
{
    if (!m_process)
        return;
    m_process->deleteLater();
    m_process = nullptr;
    m_runButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    m_pendingCarriageReturn = false;
    if (!m_log->document()->lastBlock().text().isEmpty())
        appendOutput(QStringLiteral("\n"));
    appendOutput(QStringLiteral("[%1]\n\n").arg(message));
}

// Appends process output, honouring '\r' so progress lines update in place.
void ToolTab::appendOutput(const QString &text)
{
    if (text.isEmpty())
        return;
    QScrollBar *bar = m_log->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;

    QTextCursor cursor(m_log->document());
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    qsizetype start = 0;
    while (start <= text.size()) {
        qsizetype end = start;
        while (end < text.size() && text.at(end) != QLatin1Char('\n') && text.at(end) != QLatin1Char('\r'))
            ++end;
        const QString segment = text.mid(start, end - start);
        if (!segment.isEmpty()) {
            if (m_pendingCarriageReturn) {
                cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
                m_pendingCarriageReturn = false;
            }
            cursor.insertText(segment);
        }
        if (end >= text.size())
            break;
        if (text.at(end) == QLatin1Char('\n')) {
            cursor.insertBlock();
            m_pendingCarriageReturn = false;
        } else {
            m_pendingCarriageReturn = true;
        }
        start = end + 1;
    }
    cursor.endEditBlock();

    if (atBottom)
        bar->setValue(bar->maximum());
}
