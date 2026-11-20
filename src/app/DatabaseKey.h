#pragma once

#include <QByteArray>
#include <QString>

// The key of the local database (ChatStore, SQLCipher): 32 random bytes in
// the system keyring (GNOME Keyring, KWallet) through QtKeychain, never in a
// file. The keyring is unlocked when you log in, so the app opens without
// asking anything.
//
// A new key is made only when there is no encrypted history yet: if the
// history is encrypted and its key is gone, nothing is made over it.
namespace DatabaseKey {

struct Result
{
    QByteArray key;
    QString error; // for people, when there is no key
};

// Waits for the keyring (a local event loop). `mayCreate`: no encrypted
// history exists, so a missing key may be made and kept.
Result load(bool mayCreate);

} // namespace DatabaseKey
