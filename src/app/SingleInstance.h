#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>

#include <memory>

// One running copy per history: started again (from the menu, while it sits
// in the tray), the program asks the running one to show its window and
// ends, instead of a second window, a second tray icon and the same
// database opened twice.
//
// The first holds a lock file beside the database (`<file>.lock`; a lock
// left by a crashed copy is taken over) and listens on a local socket only
// this user can reach: in the runtime folder on Linux, a named pipe on
// Windows, its name made from the database's path, so copies with other
// data folders (tests, two accounts on one computer) don't meet. Whatever
// connects only makes it show the window: nothing it sends is read.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(const QString &dataFile, QObject *parent = nullptr);
    ~SingleInstance() override;

    // True: this is the first; it listens, and `activated` says when another
    // started. False: another runs (it was asked to show itself); end.
    bool start();
    QString socketName() const { return m_name; }

signals:
    void activated();

private:
    QString m_lockPath;
    QString m_name;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer m_server;
};
