#include "appcatalog.h"

#include <QCoreApplication>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>

namespace {
// Minimal .desktop reader: only the [Desktop Entry] group, localized Name.
QHash<QString, QString> readDesktop(const QString &path)
{
    QHash<QString, QString> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    in.setCodec("UTF-8");
    bool inEntry = false;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        if (!inEntry || line.startsWith(QLatin1Char('#'))) continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) out.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return out;
}

QString findIcon(const QString &icon, const QString &desktopDir)
{
    if (icon.isEmpty()) return QString();
    if (QFileInfo(icon).isAbsolute()) return QFile::exists(icon) ? icon : QString();
    const QStringList sizes = {QStringLiteral("172x172"), QStringLiteral("128x128"), QStringLiteral("108x108"),
                               QStringLiteral("86x86"), QStringLiteral("256x256"), QStringLiteral("96x96"),
                               QStringLiteral("64x64")};
    const QStringList bases = {QDir::homePath() + QStringLiteral("/.local/share/icons/hicolor"),
                               QStringLiteral("/usr/share/icons/hicolor")};
    for (const QString &b : bases)
        for (const QString &s : sizes) {
            const QString p = QStringLiteral("%1/%2/apps/%3.png").arg(b, s, icon);
            if (QFile::exists(p)) return p;
        }
    // Sailfish system apps: icon-launcher-* in the theme, biggest scale first.
    const QDir theme(QStringLiteral("/usr/share/themes/sailfish-default/meegotouch"));
    QStringList scales = theme.entryList(QStringList() << QStringLiteral("z*"), QDir::Dirs);
    std::sort(scales.begin(), scales.end(), [](const QString &a, const QString &b) {
        return a.mid(1).toDouble() > b.mid(1).toDouble();
    });
    for (const QString &z : scales) {
        const QString p = theme.filePath(z + QStringLiteral("/icons/") + icon + QStringLiteral(".png"));
        if (QFile::exists(p)) return p;
    }
    const QString local = desktopDir + QLatin1Char('/') + icon + QStringLiteral(".png");
    return QFile::exists(local) ? local : QString();
}

bool looksLikeMedia(const QString &categories, const QString &id, const QString &name)
{
    static const QRegularExpression cat(QStringLiteral("(^|;)(Audio|Music|Player|Radio|Podcast)(;|$)"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression words(QStringLiteral("music|radio|podcast|podqast|tunebridge|mediaplayer|player|spotify|audio"),
                                          QRegularExpression::CaseInsensitiveOption);
    return cat.match(categories).hasMatch() || words.match(id).hasMatch() || words.match(name).hasMatch();
}
} // namespace

AppCatalog::AppCatalog(QObject *parent) : QObject(parent)
{
    QSettings s;
    m_selected = s.value(QStringLiteral("carApps")).toStringList();
    reload();
    if (!s.contains(QStringLiteral("carApps"))) {
        // First start: preselect the music/radio players.
        for (const AppEntry &a : m_apps)
            if (a.media && a.id != QLatin1String("harbour-sailauto")) m_selected << a.id;
    }
}

void AppCatalog::reload()
{
    m_apps.clear();
    const QStringList dirs = {QStringLiteral("/usr/share/applications"),
                              QDir::homePath() + QStringLiteral("/.local/share/applications")};
    const QString lang = QLocale().name();          // de_DE
    const QString shortLang = lang.left(2);          // de
    QSet<QString> seen;
    for (const QString &d : dirs) {
        const QDir dir(d);
        for (const QString &file : dir.entryList(QStringList() << QStringLiteral("*.desktop"), QDir::Files)) {
            const auto e = readDesktop(dir.filePath(file));
            if (e.value(QStringLiteral("Type")) != QLatin1String("Application")) continue;
            if (e.value(QStringLiteral("NoDisplay")) == QLatin1String("true")
                || e.value(QStringLiteral("Hidden")) == QLatin1String("true")) continue;
            const QString exec = e.value(QStringLiteral("Exec"));
            if (exec.isEmpty()) continue;
            AppEntry a;
            a.id = file.left(file.size() - 8);
            if (seen.contains(a.id)) continue;
            seen.insert(a.id);
            a.name = e.value(QStringLiteral("Name[%1]").arg(lang),
                             e.value(QStringLiteral("Name[%1]").arg(shortLang), e.value(QStringLiteral("Name"), a.id)));
            a.iconPath = findIcon(e.value(QStringLiteral("Icon")), d);
            a.exec = exec;
            a.media = looksLikeMedia(e.value(QStringLiteral("Categories")), a.id, a.name);
            m_apps.append(a);
        }
    }
    std::sort(m_apps.begin(), m_apps.end(), [](const AppEntry &x, const AppEntry &y) {
        return QString::localeAwareCompare(x.name, y.name) < 0;
    });
}

QVector<AppEntry> AppCatalog::selected() const
{
    // Order of selection = order on the car screen.
    QVector<AppEntry> out;
    for (const QString &id : m_selected)
        if (const AppEntry *a = find(id)) out.append(*a);
    return out;
}

void AppCatalog::setSelected(const QString &id, bool on)
{
    if (on == m_selected.contains(id)) return;
    if (on) m_selected.append(id);
    else m_selected.removeAll(id);
    QSettings().setValue(QStringLiteral("carApps"), m_selected);
    emit selectionChanged();
}

QVariantList AppCatalog::model() const
{
    QVariantList out;
    for (const AppEntry &a : m_apps) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), a.id);
        m.insert(QStringLiteral("name"), a.name);
        m.insert(QStringLiteral("icon"), a.iconPath.isEmpty() ? QString() : QStringLiteral("file://") + a.iconPath);
        m.insert(QStringLiteral("selected"), m_selected.contains(a.id));
        m.insert(QStringLiteral("media"), a.media);
        out.append(m);
    }
    return out;
}

const AppEntry *AppCatalog::find(const QString &id) const
{
    for (const AppEntry &a : m_apps)
        if (a.id == id) return &a;
    return nullptr;
}

QList<qint64> AppCatalog::pidsOf(const AppEntry &a) const
{
    // The app's binary: absolute paths in the Exec line that are not
    // launchers (invoker, sailjail, sh), else /usr/bin/<desktop id>.
    QStringList bins;
    for (const QString &t : a.exec.split(QLatin1Char(' '), QString::SkipEmptyParts)) {
        const QString tok = QString(t).remove(QLatin1Char('"'));
        if (!tok.startsWith(QLatin1Char('/'))) continue;
        const QString base = tok.section(QLatin1Char('/'), -1);
        if (base == QLatin1String("invoker") || base == QLatin1String("sailjail") || base == QLatin1String("sh")
            || base.endsWith(QLatin1String(".desktop")))
            continue;
        bins << base;
    }
    if (bins.isEmpty()) bins << a.id;
    const qint64 self = QCoreApplication::applicationPid();
    QList<qint64> pids;
    const QDir proc(QStringLiteral("/proc"));
    for (const QString &pid : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool num = false;
        const qint64 n = pid.toLongLong(&num);
        if (!num || n == self) continue;
        QFile f(proc.filePath(pid + QStringLiteral("/cmdline")));
        if (!f.open(QIODevice::ReadOnly)) continue;
        // Any argument: sailjail/invoker/python wrappers carry the program path later.
        for (const QByteArray &arg : f.readAll().split('\0')) {
            const QString base = QString::fromLocal8Bit(arg).section(QLatin1Char('/'), -1);
            if (bins.contains(base)) { pids << n; break; }
        }
    }
    return pids;
}

bool AppCatalog::isRunning(const AppEntry &a) const
{
    return !pidsOf(a).isEmpty();
}

int AppCatalog::terminate(const QString &id) const
{
    const AppEntry *a = find(id);
    if (!a) return 0;
    int n = 0;
    for (qint64 pid : pidsOf(*a))
        if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0) ++n;
    return n;
}

AppCatalog::LaunchResult AppCatalog::launch(const QString &id, QString *error)
{
    const AppEntry *a = find(id);
    if (!a) {
        if (error) *error = QStringLiteral("App %1 nicht gefunden").arg(id);
        return Failed;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastLaunch.value(id, 0) < 3000) return TooSoon; // double tap
    if (isRunning(*a)) return AlreadyRunning;
    // Drop the field codes (%u %F ...), keep the rest of the Exec line as is.
    QString cmd = a->exec;
    cmd.remove(QRegularExpression(QStringLiteral("\\s%[fFuUdDnNickvm]")));
    cmd.replace(QStringLiteral("%%"), QStringLiteral("%"));
    // Apps must not inherit our files (USB accessory, sockets): they would keep
    // them open and block the next car connection.
    {
        const QDir fds(QStringLiteral("/proc/self/fd"));
        for (const QString &e : fds.entryList(QDir::System | QDir::NoDotAndDotDot)) {
            bool ok = false;
            const int fd = e.toInt(&ok);
            if (!ok || fd <= 2) continue;
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), QStringList() << QStringLiteral("-c") << cmd)) {
        if (error) *error = QStringLiteral("Start fehlgeschlagen: %1").arg(cmd);
        return Failed;
    }
    m_lastLaunch.insert(id, now);
    if (!m_started.contains(id)) m_started << id;
    return Started;
}
