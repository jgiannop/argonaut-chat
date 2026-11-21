#pragma once

#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtGlobal>

// The first line of a test's main(), before the application object: a
// private HOME and XDG folders (settings and the chat store never touch the
// real ones), and no display. Tests keep their files under home(): on
// Windows QDir::homePath() is the real profile, whatever HOME says.
namespace TestSupport {

inline QString &homeFolder()
{
    static QString folder;
    return folder;
}
inline QString home() { return homeFolder(); }

class Sandbox
{
public:
    Sandbox()
    {
        homeFolder() = m_dir.path();
        const QByteArray root = m_dir.path().toUtf8();
        qputenv("HOME", root);
        qputenv("XDG_CONFIG_HOME", root + "/config");
        qputenv("XDG_DATA_HOME", root + "/data");
        qputenv("XDG_CACHE_HOME", root + "/cache");
        qputenv("XDG_RUNTIME_DIR", root + "/runtime");
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("ARGONAUT_THEMES_PATH", "");
#ifdef Q_OS_WIN
        // Windows takes its folders from the system, not from the variables
        // above: Qt's test folders, and settings in a file here, not the registry.
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path() + QStringLiteral("/config"));
#endif
    }
    QString path() const { return m_dir.path(); }

private:
    QTemporaryDir m_dir;
};

} // namespace TestSupport

#define CHAT_TEST_MAIN(TestClass, AppClass)                                                                            \
    int main(int argc, char **argv)                                                                                    \
    {                                                                                                                  \
        TestSupport::Sandbox sandbox;                                                                                  \
        AppClass app(argc, argv);                                                                                      \
        AppClass::setOrganizationName(QStringLiteral("ArgonautTests"));                                                \
        AppClass::setApplicationName(QStringLiteral("argonaut-chat-tests"));                                           \
        TestClass test;                                                                                                \
        return QTest::qExec(&test, argc, argv);                                                                        \
    }
