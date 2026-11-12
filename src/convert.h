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

// What ffprobe says about the chosen file.
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

// Where a file is picked: drop it, or click.
class FileDrop : public QFrame
{
    Q_OBJECT

public:
    explicit FileDrop(QWidget *parent = nullptr);
    void setFile(const QString &name, const QString &details, bool error = false);
    void setHighlighted(bool on);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    ElidedLabel *m_title;
    ElidedLabel *m_details;
    QPushButton *m_button;
    bool m_highlighted = false;
};

// The FFMPEG screen: one file in, ready-made conversions out.
class ConvertPage : public QWidget
{
    Q_OBJECT

public:
    explicit ConvertPage(QWidget *parent = nullptr);
    ~ConvertPage() override;

    void setInput(const QString &path);

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
    void browse();
    void refresh();
    void saveOptions();
    void run();
    void launch();
    void stop();
    void readProgress();
    void finished(bool started, int exitCode);
    void setStatus(const QString &text, bool error = false);
    void setRunning(bool running);
    void shutDown();

    MediaInfo m_info;
    QString m_inputError;
    QProcess *m_probe = nullptr;

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

    QProcess *m_process = nullptr;
    ConvertOptions m_runOptions; // the choices the running job started with
    ConvertPlan m_runPlan;
    QString m_output;
    QString m_doneFile;
    QByteArray m_progressBuffer;
    QByteArray m_errors;
    QElapsedTimer m_started;
    int m_maxKbps = 0;
    int m_attempt = 0;
    bool m_stopping = false;
    bool m_loading = false;
};
