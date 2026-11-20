#include "RateLimiter.h"

#include <QDateTime>

#include <limits>

#include <cmath>

RateLimiter::RateLimiter(Clock clock)
{
    setClock(std::move(clock));
}

void RateLimiter::setClock(Clock clock)
{
    m_clock = clock ? std::move(clock) : Clock([] { return QDateTime::currentMSecsSinceEpoch(); });
}

RateLimiter::Bucket &RateLimiter::refill(const QString &key, const Rule &rule)
{
    const qint64 now = m_clock();
    auto it = m_buckets.find(key);
    if (it == m_buckets.end()) {
        it = m_buckets.insert(key, Bucket{rule.capacity, now, rule.capacity, rule.perSecond});
        return *it;
    }
    Bucket &bucket = *it;
    const double elapsed = double(qMax<qint64>(0, now - bucket.last)) / 1000.0;
    bucket.tokens = qMin(rule.capacity, bucket.tokens + elapsed * rule.perSecond);
    bucket.last = now;
    bucket.capacity = rule.capacity;
    bucket.perSecond = rule.perSecond;
    return bucket;
}

bool RateLimiter::take(const QString &key, const Rule &rule, qint64 *retryAfterMs, double cost)
{
    Bucket &bucket = refill(key, rule);
    if (bucket.tokens >= cost) {
        bucket.tokens -= cost;
        return true;
    }
    if (retryAfterMs)
        *retryAfterMs = rule.perSecond > 0 && cost <= rule.capacity
                            ? qint64(std::ceil((cost - bucket.tokens) / rule.perSecond * 1000.0))
                            : std::numeric_limits<qint64>::max();
    return false;
}

bool RateLimiter::wouldAllow(const QString &key, const Rule &rule)
{
    return refill(key, rule).tokens >= 1.0;
}

void RateLimiter::prune()
{
    const qint64 now = m_clock();
    for (auto it = m_buckets.begin(); it != m_buckets.end();) {
        const double elapsed = double(qMax<qint64>(0, now - it->last)) / 1000.0;
        if (it->tokens + elapsed * it->perSecond >= it->capacity)
            it = m_buckets.erase(it); // full again: as if never used
        else
            ++it;
    }
}
