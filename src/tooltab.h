#pragma once

#include "tools.h"

#include <QHash>
#include <QStringDecoder>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QVBoxLayout;

// One tab: the options of a command-line tool rendered as widgets, plus the
// command preview, the run controls and the output log.
class ToolTab : public QWidget
{
    Q_OBJECT

public:
    enum Kind { YtDlp, Ffmpeg };

    explicit ToolTab(Kind kind, QWidget *parent = nullptr);
    ~ToolTab() override;

    QStringList buildArguments() const;
    void setFilter(const QString &text);

signals:
    void optionsLoaded();

private:
    struct Row {
        OptionDef def;
        int section = 0;
        QWidget *widget = nullptr;
        QCheckBox *check = nullptr;
        QLineEdit *value = nullptr;
        QLineEdit *spec = nullptr;
        QComboBox *where = nullptr;
    };
    struct Section {
        QString title;
        QWidget *container = nullptr;
        QPushButton *header = nullptr;
        QWidget *body = nullptr;
        bool expanded = false;
        QList<int> rows;
    };

    void loadOptions();
    void clearOptions();
    void showMessage(const QString &text, bool offerChooser);
    void buildOptionWidgets(const QList<OptionSection> &sections);
    QWidget *createRow(int index);
    void addValueHelpers(Row &row, class QHBoxLayout *layout);
    void applyFilter();
    void toggleSection(int index);
    void updateSectionHeader(int index);
    void optionChanged(int rowIndex);
    void updatePreview();
    void resetOptions();
    void chooseProgram();

    void run();
    void stop();
    void processDone(const QString &message);
    void appendOutput(const QString &text);
    QString workingDirectory() const;
    QList<ListEntry> capabilityList(const QString &listArg, int nameColumn, bool hasSeparator,
                                    int headerLines, int flagWidth);

    const Kind m_kind;
    const QString m_toolName;
    QString m_program;

    QLineEdit *m_urlEdit = nullptr;    // yt-dlp
    QLineEdit *m_dirEdit = nullptr;    // yt-dlp
    QLineEdit *m_inputEdit = nullptr;  // ffmpeg
    QLineEdit *m_outputEdit = nullptr; // ffmpeg
    QLineEdit *m_filterEdit = nullptr;
    QLineEdit *m_extraEdit = nullptr;
    QLineEdit *m_commandEdit = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QWidget *m_optionsHost = nullptr;
    QVBoxLayout *m_optionsLayout = nullptr;

    QList<Row> m_rows;
    QList<Section> m_sections;
    QHash<QString, QList<ListEntry>> m_listCache;

    QProcess *m_process = nullptr;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    bool m_pendingCarriageReturn = false;
};
