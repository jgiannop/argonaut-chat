#include "AppLock.h"

#include "ChatStore.h"
#include "Crypto.h"

#include <QDateTime>

AppLock::AppLock(ChatStore *store)
    : m_store(store)
{
}

qint64 AppLock::now() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

bool AppLock::isSet() const
{
    return !m_store->appLock().hash.isEmpty();
}

int AppLock::idleMinutes() const
{
    return m_store->appLock().idleMinutes;
}

qint64 AppLock::waitMs() const
{
    return qMax(qint64(0), m_store->appLock().waitUntil - now());
}

bool AppLock::check(const QString &passphrase, QString *error)
{
    ChatStore::AppLockState state = m_store->appLock();
    if (state.hash.isEmpty())
        return true;
    if (const qint64 wait = waitMs(); wait > 0) {
        if (error)
            *error = tr("Too many wrong tries: wait %n second(s).", nullptr, int((wait + 999) / 1000));
        return false;
    }
    if (Crypto::verifyPassphrase(state.hash, passphrase)) {
        if (state.failures != 0 || state.waitUntil != 0) {
            state.failures = 0;
            state.waitUntil = 0;
            m_store->setAppLock(state);
        }
        return true;
    }
    ++state.failures;
    if (state.failures >= MaxFreeTries) {
        const int doublings = qMin(state.failures - MaxFreeTries, 10);
        state.waitUntil = now() + qMin(FirstWaitMs << doublings, MaxWaitMs);
    }
    m_store->setAppLock(state);
    if (error)
        *error = state.failures >= MaxFreeTries
                     ? tr("Wrong passphrase. Wait %n second(s) before the next try.", nullptr, int((waitMs() + 999) / 1000))
                     : tr("Wrong passphrase.");
    return false;
}

bool AppLock::set(const QString &current, const QString &passphrase, int idleMinutes, QString *error)
{
    if (isSet() && !check(current, error))
        return false;
    if (passphrase.size() < MinLength) {
        if (error)
            *error = tr("The passphrase needs at least %1 characters.").arg(MinLength);
        return false;
    }
    const QString hash = Crypto::hashPassphrase(passphrase);
    if (hash.isEmpty()) {
        if (error)
            *error = tr("The passphrase could not be kept.");
        return false;
    }
    m_store->setAppLock({hash, qMax(0, idleMinutes), 0, 0});
    return true;
}

bool AppLock::setIdleMinutes(const QString &current, int idleMinutes, QString *error)
{
    if (!check(current, error))
        return false;
    ChatStore::AppLockState state = m_store->appLock();
    if (state.hash.isEmpty())
        return false;
    state.idleMinutes = qMax(0, idleMinutes);
    m_store->setAppLock(state);
    return true;
}

bool AppLock::turnOff(const QString &current, QString *error)
{
    if (!check(current, error))
        return false;
    forget();
    return true;
}

void AppLock::forget()
{
    m_store->setAppLock({});
}
