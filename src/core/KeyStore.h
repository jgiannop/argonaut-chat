#pragma once

#include "Session.h"

#include <QSqlDatabase>
#include <QString>
#include <QUrl>

#include <optional>

class ChatStore;

// This device's secrets and what it knows of others, in the chat store's
// file (so the same protection covers them: 0600 now, encrypted in step 5):
//
//   - the account: username, device number, server, identity key pair
//   - signed and one-time prekeys, with their secret halves
//   - one Session per device of each person
//   - the identity key seen for each person (trust on first use): a change
//     is recorded, and the safety number is shown as changed until verified.
class KeyStore
{
public:
    struct Account
    {
        QString username;
        quint32 device = 1;
        QUrl server;
        Crypto::IdentityKeyPair identity;
        bool registered = false; // the server said yes
    };

    enum class Trust {
        New,       // never seen: kept
        Same,      // the key we knew
        Changed,   // another key than before: kept, and the change recorded
    };

    explicit KeyStore(ChatStore *store);

    std::optional<Account> account() const;
    bool saveAccount(const Account &account);
    bool setRegistered();
    // Everything of the account (after a refused sign-up).
    void forgetAccount();

    bool addSignedPreKey(const X3dh::SignedPreKey &key);
    std::optional<X3dh::SignedPreKey> signedPreKey(quint32 id) const;
    std::optional<X3dh::SignedPreKey> latestSignedPreKey() const;
    bool addOneTimePreKeys(const QList<X3dh::OneTimePreKey> &keys);
    std::optional<X3dh::OneTimePreKey> oneTimePreKey(quint32 id) const;
    void removeOneTimePreKey(quint32 id);
    int oneTimePreKeyCount() const;
    // Ids go up and are never used twice.
    quint32 takePreKeyIds(int count);

    std::optional<Session> session(const QString &username, quint32 device) const;
    bool saveSession(const QString &username, quint32 device, const Session &session);
    QList<quint32> sessionDevices(const QString &username) const;
    void removeSession(const QString &username, quint32 device);

    // Called with every identity key met for someone.
    Trust checkIdentity(const QString &username, const QByteArray &identityKey);
    QByteArray identityKey(const QString &username) const;
    // The key changed and the user has not looked at the new safety number.
    bool identityChanged(const QString &username) const;
    void setVerified(const QString &username, bool verified);
    bool isVerified(const QString &username) const;

private:
    void createTables();

    QSqlDatabase m_db;
};
