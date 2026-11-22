#pragma once

#include <QCoreApplication>
#include <QString>

#include <functional>

class ChatStore;

// The app lock: a passphrase that hides the chats (the window shows only a
// field for it) after some idle minutes, at start, or when asked. Its
// Argon2id hash and the settings are in the encrypted database
// (ChatStore::appLock), never the passphrase.
//
// What it is for: someone at this computer while you are away. Not more:
// whoever can use your session can also read the keyring, and with it the
// database. A forgotten passphrase: `argonaut-chat --forget-app-lock`.
//
// Wrong tries: after MaxFreeTries in a row, each needs a wait first (30 s,
// doubling, at most 15 min), kept across restarts.
class AppLock
{
    Q_DECLARE_TR_FUNCTIONS(AppLock)

public:
    static constexpr int MinLength = 6;
    static constexpr int MaxFreeTries = 5;
    static constexpr qint64 FirstWaitMs = 30 * 1000;
    static constexpr qint64 MaxWaitMs = 15 * 60 * 1000;

    explicit AppLock(ChatStore *store);
    // Tests: the clock (ms).
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }

    bool isSet() const;
    int idleMinutes() const; // 0: no locking when idle

    // Turns it on (or changes it: `current` must be right). False and `error` if not.
    bool set(const QString &current, const QString &passphrase, int idleMinutes, QString *error = nullptr);
    bool setIdleMinutes(const QString &current, int idleMinutes, QString *error = nullptr);
    bool turnOff(const QString &current, QString *error = nullptr);
    // Without the passphrase (--forget-app-lock: whoever runs that can read the database anyway).
    void forget();

    // The passphrase right: true (and the wrong tries forgotten). Wrong, or
    // too soon after too many wrong ones: false, and `error` says which.
    bool check(const QString &passphrase, QString *error = nullptr);
    // ms until the next try is allowed (0: now).
    qint64 waitMs() const;

private:
    qint64 now() const;

    ChatStore *m_store;
    std::function<qint64()> m_clock;
};
