#pragma once

#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// One command-line option of yt-dlp or ffmpeg, as read from the tool's own help.
struct OptionDef {
    enum Position { Global, Input, Output, Either };

    QString label;              // text shown next to the checkbox, e.g. "-f, --format"
    QStringList args;           // arguments emitted when enabled (the value follows)
    QString argName;            // name of the value, used as placeholder
    bool hasValue = false;      // show a value field
    bool valueOptional = false; // emit the option alone when the value is empty
    bool multiValue = false;    // the value is a space-separated list of arguments
    QStringList choices;        // allowed values, when the help lists them
    bool streamSpec = false;    // ffmpeg: accepts ":<stream_spec>"
    Position position = Global; // ffmpeg: where it goes on the command line
    bool defaultInput = false;  // ffmpeg: an Either option that normally applies to the input
    QString description;
};

struct OptionSection {
    QString title;
    QList<OptionDef> options;
};

// A line of an ffmpeg capability list (-encoders, -formats, -filters ...).
struct ListEntry {
    QString name;
    QString flags;
    QString description;
};

QList<OptionSection> parseYtDlpHelp(const QString &text, QStringList *unparsed = nullptr);
QList<OptionSection> parseFfmpegHelp(const QString &text, QStringList *unparsed = nullptr);
// Help printed by Python's argparse (spotdl, instaloader).
QList<OptionSection> parseArgparseHelp(const QString &text, QStringList *unparsed = nullptr);

// nameColumn: index of the whitespace-separated column holding the name.
// hasSeparator: entries start after a line made of dashes; otherwise skip headerLines.
// flagWidth: when > 0 the flags are a fixed-width prefix (they may contain spaces).
QList<ListEntry> parseFfmpegList(const QString &text, int nameColumn, bool hasSeparator,
                                 int headerLines = 0, int flagWidth = 0);

// Locates yt-dlp / ffmpeg: saved override, PATH, then the usual install folders.
QString findTool(const QString &name);
// Environment for child processes: a GUI app does not inherit the shell PATH.
QProcessEnvironment toolEnvironment();
// Runs a tool and returns its merged output (used for the short list queries).
QString runTool(const QString &program, const QStringList &args, int timeoutMs = 15000);

QString shellQuote(const QString &arg);
