#pragma once

#include <QHash>
#include <QString>

#include <functional>

// Token buckets: each key (an address, an account, ...) holds up to
// `capacity` tokens, refilled at `perSecond`; a request takes one. A full
// bucket allows a burst, then the steady rate.
class RateLimiter
{
public:
    struct Rule
    {
        double capacity = 1;
        double perSecond = 1;
    };

    using Clock = std::function<qint64()>; // milliseconds

    explicit RateLimiter(Clock clock = {});

    // True and a token taken; false and, in `retryAfterMs`, when the next one comes.
    bool take(const QString &key, const Rule &rule, qint64 *retryAfterMs = nullptr);
    // Without taking.
    bool wouldAllow(const QString &key, const Rule &rule);
    // Forgets the buckets that have been full for a while (memory).
    void prune();
    int size() const { return int(m_buckets.size()); }

    void setClock(Clock clock);

private:
    struct Bucket
    {
        double tokens = 0;
        qint64 last = 0;
        double capacity = 0;
        double perSecond = 0;
    };

    Bucket &refill(const QString &key, const Rule &rule);

    Clock m_clock;
    QHash<QString, Bucket> m_buckets;
};
