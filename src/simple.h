#pragma once

#include "widgets.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QList>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSlider;
class QToolButton;
class QVBoxLayout;

// What to download and how; every row keeps the choices it was added with.
struct DownloadSettings {
    enum Mode { Both, VideoOnly, AudioOnly };

    Mode mode = Both;
    QString videoFormat = QStringLiteral("mp4");
    int height = 1080; // 0: the best available
    QString audioFormat = QStringLiteral("mp3");
    int bitrate = 192; // kbps
};

// One entry of the download list: title, progress, and its own buttons.
class DownloadRow : public QFrame
{
    Q_OBJECT

public:
    enum State { Resolving, Ready, Queued, Running, Done, Failed };
    enum Engine { YtDlp, SpotDl, Instaloader };

    DownloadRow(Service service, Engine engine, const QString &target, const DownloadSettings &settings,
                QWidget *parent = nullptr);
    ~DownloadRow() override;

    State state() const { return m_state; }
    Service service() const { return m_service; }
    const DownloadSettings &settings() const { return m_settings; }
    QStringList arguments(const QString &directory) const;

    void setTarget(Engine engine, const QString &target);
    // The link was read: the row can be downloaded.
    void setInfo(const QString &title, const QString &author, int seconds);
    void setFailed(const QString &message);
    void setQueued();
    void start(const QString &directory);
    void stop();

signals:
    void downloadRequested();
    void removeRequested();
    void stateChanged();

protected:
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void setState(State state, const QString &status = {});
    void refresh();
    void updateSubtitle();
    void setBusy(const QString &status);
    void consume(const QString &text);
    void handleLine(const QString &line);
    void handleYtDlpLine(const QString &line);
    void finished(bool started, int exitCode);
    void reveal();

    const Service m_service;
    Engine m_engine;
    QString m_target;
    DownloadSettings m_settings;
    State m_state = Resolving;

    QString m_title;
    QString m_author;
    int m_seconds = 0;
    QString m_error;

    ElidedLabel *m_titleLabel = nullptr;
    ElidedLabel *m_subtitleLabel = nullptr;
    QProgressBar *m_bar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QToolButton *m_actionButton = nullptr;
    QToolButton *m_removeButton = nullptr;

    QProcess *m_process = nullptr;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    QString m_buffer;
    QString m_directory;
    QString m_filePath;
    QString m_lastLine;
    bool m_stopping = false;
    bool m_succeeded = false;   // spotdl: a track was written
    double m_expectedBytes = 0; // every file of this entry
    double m_doneBytes = 0;     // files already complete
    double m_fileBytes = 0;
    double m_fileTotal = 0;
    QElapsedTimer m_lastUpdate;
};

// The default screen: paste links, pick format and quality, download the list.
class SimplePage : public QWidget
{
    Q_OBJECT

public:
    explicit SimplePage(QWidget *parent = nullptr);

    // The row of site buttons; the window places it next to the mode switch.
    QWidget *serviceBar() const { return m_serviceBar; }
    void addFromText(const QString &text);

private:
    void setService(Service service);
    DownloadSettings currentSettings(Service service) const;
    void updateOptionStates();
    void saveOptions();

    void addTarget(Service service, const QString &target);
    DownloadRow *addRow(Service service, DownloadRow::Engine engine, const QString &target,
                        const DownloadSettings &settings, int index = -1);
    void removeRow(DownloadRow *row);
    void resolveWithYtDlp(DownloadRow *row, const QString &url, const QString &instagramPost);
    void resolveWithSpotDl(DownloadRow *row, const QString &target);

    void downloadAll();
    void clearAll();
    void schedulePump();
    void pump();
    void updateSummary();
    QString directory() const;

    Service m_service = Service::YouTube;
    QWidget *m_serviceBar;
    QButtonGroup *m_serviceGroup;
    QLineEdit *m_linkEdit;
    qsizetype m_linkLength = 0;

    QCheckBox *m_videoCheck;
    QComboBox *m_videoFormat;
    QSlider *m_videoQuality;
    QLabel *m_videoQualityLabel;
    QCheckBox *m_audioCheck;
    QComboBox *m_audioFormat;
    QSlider *m_audioQuality;
    QLabel *m_audioQualityLabel;

    QScrollArea *m_scroll;
    QLabel *m_emptyLabel;
    QVBoxLayout *m_rowsLayout;
    QList<DownloadRow *> m_rows;

    QLineEdit *m_dirEdit;
    QLabel *m_summaryLabel;
    QPushButton *m_clearButton;
    QPushButton *m_downloadAllButton;

    QTemporaryDir m_tempDir;
    int m_tempCounter = 0;
    bool m_pumpScheduled = false;
};
