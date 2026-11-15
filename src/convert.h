#pragma once

#include "widgets.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QStackedLayout;
class QToolButton;
class QVBoxLayout;
struct ConvertItem;

// What ffprobe says about a chosen file.
struct MediaInfo {
    bool valid = false;
    QString path;
    double duration = 0; // seconds; 0 when not known
    qint64 size = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool hasCover = false; // picture attached to an audio file
    QString videoCodec;
    int width = 0;
    int height = 0;
    QString audioCodec;
    int audioKbps = 0;
    int sampleRate = 0;
    int channels = 0;
};

struct ConvertOptions {
    QString format;  // extension of the result; empty: the same as the input
    int limitMb = 0; // 0, or the size the audio has to fit in (WhatsApp)
    bool denoise = false;
    bool voice = false;
    bool normalize = false;
    bool trimSilence = false;
    bool mono = false;
};

// The ffmpeg command for a file and a set of choices.
struct ConvertPlan {
    QString error;          // not empty: nothing to run
    bool nothingToDo = false; // the error only says that no option is chosen
    QString extension;
    bool audioOnly = false;
    QStringList tags;       // added to the file name, e.g. "voce", "whatsapp16"
    QStringList arguments;  // between "-i input" and the output file
    QString details;        // shown to the user, e.g. "MP3 48 kbps mono"
    int kbps = 0;           // audio bitrate, when it is chosen here
    qint64 estimatedBytes = 0;
};

// One file of the list in the box: its name, what it is, how far it has got.
class FileRow : public QFrame
{
    Q_OBJECT

public:
    FileRow(const QString &name, const QString &path, QWidget *parent = nullptr);
    void setDetails(const QString &text, bool error = false);
    // permille < 0: at work, but how far is not known.
    void setProgress(int permille, const QString &status);
    void setDone(bool done);     // shows the button that opens the folder
    void setLocked(bool locked); // nothing is removed while the list is converted

signals:
    void revealRequested();
    void removeRequested();

protected:
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void refreshIcons();

    ElidedLabel *m_title;
    ElidedLabel *m_details;
    QProgressBar *m_bar;
    ElidedLabel *m_status;
    QToolButton *m_revealButton = nullptr;
    QToolButton *m_removeButton = nullptr;
};

// Where the files are picked: drop them, or click. One file fills the box;
// several become a list that scrolls.
class FileDrop : public QFrame
{
    Q_OBJECT

public:
    explicit FileDrop(QWidget *parent = nullptr);
    // The text of the box while it holds one file, or none.
    void setFile(const QString &name, const QString &details, bool error = false);
    void addRow(FileRow *row);
    void removeRow(FileRow *row);
    // How many files there are: from two on, the list takes the place of the big text.
    void setCount(int count);
    void showRow(FileRow *row);
    void setHighlighted(bool on);
    void setBusy(bool busy); // no file is chosen while the list is converted

signals:
    void clicked();      // choose the files, in place of the ones there are
    void addClicked();   // choose more files
    void clearClicked(); // empty the list

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QStackedLayout *m_stack;
    QWidget *m_single;
    ElidedLabel *m_title;
    ElidedLabel *m_details;
    QPushButton *m_button;
    QLabel *m_countLabel;
    QPushButton *m_addButton;
    QPushButton *m_clearButton;
    QScrollArea *m_scroll;
    QVBoxLayout *m_rows;
    bool m_highlighted = false;
    bool m_busy = false;
};

// The FFMPEG screen: files in, ready-made conversions out, one file at a time.
class ConvertPage : public QWidget
{
    Q_OBJECT

public:
    explicit ConvertPage(QWidget *parent = nullptr);
    ~ConvertPage() override;

    // These files in place of the ones in the list.
    void setInputs(const QStringList &paths);
    // More files for the list; the ones already there are not added twice.
    void addInputs(const QStringList &paths);

    static MediaInfo parseProbe(const QByteArray &json, const QString &path);
    // maxKbps: upper bound for the audio bitrate (a result that came out too big is redone lower).
    static ConvertPlan plan(const MediaInfo &info, const ConvertOptions &options, int maxKbps = 0);
    // "name_tags.ext" in the folder, numbered when the name is taken.
    static QString outputPath(const QString &input, const QString &directory, const ConvertPlan &plan);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    ConvertOptions options() const;
    QString directory() const;
    void browse(bool add);
    void removeItem(ConvertItem *item);
    void clearItems();
    void probeNext();
    void endProbe(ConvertItem *item, const QString &error);
    void updateRow(ConvertItem *item, const ConvertPlan &plan);
    void choicesChanged();
    void refresh();
    void saveOptions();
    void run();
    void startNext();
    QString launch();
    void stop();
    void readProgress();
    void finished(bool started, int exitCode);
    void failCurrent(const QString &error);
    void finishRun();
    double weight(const ConvertItem *item) const;
    void resetStatus();
    void setStatus(const QString &text, bool error = false);
    void setRunning(bool running);
    void shutDown();

    QList<ConvertItem *> m_items; // the files of the list, in the order they were added
    int m_probing = 0;            // how many ffprobe are reading a file right now

    FileDrop *m_drop;
    QComboBox *m_format;
    QLineEdit *m_otherFormat;
    QCheckBox *m_limit64;
    QCheckBox *m_limit16;
    QCheckBox *m_denoise;
    QCheckBox *m_voice;
    QCheckBox *m_normalize;
    QCheckBox *m_trimSilence;
    QCheckBox *m_mono;
    QLineEdit *m_dirEdit;
    QPushButton *m_dirButton;
    ElidedLabel *m_resultLabel;
    QProgressBar *m_bar;
    ElidedLabel *m_statusLabel;
    QPushButton *m_revealButton;
    QPushButton *m_runButton;

    // --- the run: the files are converted in turn, never two together --------
    QList<ConvertItem *> m_queue;    // still waiting for their turn
    ConvertItem *m_current = nullptr;
    QProcess *m_process = nullptr;
    ConvertOptions m_runOptions;     // the choices the run started with
    QString m_runDirectory;
    ConvertPlan m_runPlan;
    QString m_output;
    QString m_doneFile;              // the last file written
    QString m_lastError;
    QByteArray m_progressBuffer;
    QByteArray m_errors;
    QElapsedTimer m_started;         // the file at work
    QElapsedTimer m_runStarted;      // the whole run
    int m_runTotal = 0;
    int m_runIndex = 0;              // 1 for the first file
    int m_runDone = 0;
    int m_runFailed = 0;
    qint64 m_runBytes = 0;
    bool m_weighByDuration = false;  // a long file counts for more in the bar
    double m_weightTotal = 0;
    double m_weightDone = 0;
    int m_maxKbps = 0;
    int m_attempt = 0;
    bool m_stopping = false;
    bool m_loading = false;
};
