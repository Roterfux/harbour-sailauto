// Phone apps that can be started from the car screen. Reads the installed
// .desktop files, keeps the user's choice (QSettings "carApps") and starts
// an app with its Exec line. Showing the app itself on the car is not
// possible on Sailfish; the car screen controls it through MPRIS instead.
#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

struct AppEntry {
    QString id;        // desktop file name without ".desktop"
    QString name;
    QString iconPath;  // file path, empty if none found
    QString exec;
    bool media = false; // music/radio/podcast player (car switches to the music page)
};

class AppCatalog : public QObject
{
    Q_OBJECT
public:
    explicit AppCatalog(QObject *parent = nullptr);

    void reload();
    const QVector<AppEntry> &all() const { return m_apps; }
    QVector<AppEntry> selected() const;
    bool isSelected(const QString &id) const { return m_selected.contains(id); }
    void setSelected(const QString &id, bool on);
    // For QML: [{id, name, icon, selected, media}]
    QVariantList model() const;
    const AppEntry *find(const QString &id) const;
    bool launch(const QString &id, QString *error);

signals:
    void selectionChanged();

private:
    QVector<AppEntry> m_apps;
    QStringList m_selected;
};
