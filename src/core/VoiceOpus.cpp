// Voice messages as Ogg Opus, through Xiph.org's libraries: libopusenc to
// write (Opus in Ogg pages, RFC 7845), opusfile to read. Not in the server's
// build (Voice.cpp has the stand-ins there).
#include "Voice.h"

#include <QtEndian>

#include <ogg/ogg.h>
#include <opusenc.h>
#include <opusfile.h>

#include <cstring>
#include <memory>

static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "samples are kept little-endian and handed to Opus as they are");

namespace Voice {

namespace {

constexpr int OpusRate = 48000; // what Opus always decodes to
constexpr int Complexity = 10;  // the best; a voice note is small work

int appendPage(void *user, const unsigned char *data, opus_int32 length)
{
    static_cast<QByteArray *>(user)->append(reinterpret_cast<const char *>(data), length);
    return 0;
}

int closeStream(void *)
{
    return 0;
}

// Every byte in whole Ogg pages that pass their checksum, one stream, its
// pages numbered 0, 1, 2… (opusfile itself skips a damaged page quietly).
bool cleanPages(const QByteArray &ogg)
{
    ogg_sync_state sync;
    ogg_sync_init(&sync);
    char *buffer = ogg_sync_buffer(&sync, long(ogg.size()));
    if (!buffer) {
        ogg_sync_clear(&sync);
        return false;
    }
    memcpy(buffer, ogg.constData(), size_t(ogg.size()));
    ogg_sync_wrote(&sync, long(ogg.size()));
    ogg_page page;
    qsizetype used = 0;
    long expected = 0;
    int serial = 0;
    bool ok = true;
    for (;;) {
        const long step = ogg_sync_pageseek(&sync, &page);
        if (step == 0)
            break; // the rest is not a whole page
        if (step < 0) {
            ok = false; // bytes skipped: not a page, or a bad checksum
            break;
        }
        used += step;
        if (expected == 0)
            serial = ogg_page_serialno(&page);
        if (ogg_page_serialno(&page) != serial || ogg_page_pageno(&page) != expected) {
            ok = false;
            break;
        }
        ++expected;
    }
    ogg_sync_clear(&sync);
    return ok && expected > 0 && used == ogg.size();
}

} // namespace

struct OpusWriter::Private
{
    QByteArray out;
    OggOpusComments *comments = nullptr;
    OggOpusEnc *encoder = nullptr;
    bool failed = false;

    ~Private()
    {
        if (encoder)
            ope_encoder_destroy(encoder);
        if (comments)
            ope_comments_destroy(comments);
    }
};

OpusWriter::OpusWriter(int sampleRate)
{
    if (sampleRate < 8000 || sampleRate > OpusRate)
        return;
    auto p = std::make_unique<Private>();
    // No tags but the encoder's name (libopusenc puts it in): nothing about
    // who, where or when.
    p->comments = ope_comments_create();
    if (!p->comments)
        return;
    static const OpusEncCallbacks callbacks{appendPage, closeStream};
    int error = OPE_OK;
    p->encoder = ope_encoder_create_callbacks(&callbacks, &p->out, p->comments, sampleRate, 1, 0, &error);
    if (!p->encoder || error != OPE_OK || ope_encoder_ctl(p->encoder, OPUS_SET_BITRATE(OpusBitrate)) != OPE_OK
        || ope_encoder_ctl(p->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPE_OK
        || ope_encoder_ctl(p->encoder, OPUS_SET_COMPLEXITY(Complexity)) != OPE_OK)
        return;
    d = std::move(p);
}

OpusWriter::~OpusWriter() = default;

bool OpusWriter::isValid() const
{
    return d && !d->failed;
}

bool OpusWriter::write(const QByteArray &samples)
{
    if (!isValid() || samples.size() % 2 != 0) {
        if (d)
            d->failed = true;
        return false;
    }
    if (samples.isEmpty())
        return true;
    const auto *pcm = reinterpret_cast<const opus_int16 *>(samples.constData());
    if (ope_encoder_write(d->encoder, pcm, int(samples.size() / 2)) != OPE_OK) {
        d->failed = true;
        return false;
    }
    m_samples += samples.size() / 2;
    return true;
}

QByteArray OpusWriter::finish()
{
    if (!isValid() || m_samples == 0 || ope_encoder_drain(d->encoder) != OPE_OK) {
        d.reset();
        return {};
    }
    QByteArray out = std::move(d->out);
    d.reset();
    return out;
}

QByteArray toOpus(const QByteArray &samples, int sampleRate)
{
    OpusWriter writer(sampleRate);
    if (samples.isEmpty() || !writer.write(samples))
        return {};
    return writer.finish();
}

qint64 opusDurationMs(const QByteArray &ogg)
{
    if (ogg.size() < 4 || !ogg.startsWith("OggS") || !cleanPages(ogg))
        return -1;
    int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> file(
        op_open_memory(reinterpret_cast<const unsigned char *>(ogg.constData()), size_t(ogg.size()), &error), op_free);
    if (!file || error != 0 || op_link_count(file.get()) != 1)
        return -1;
    const int channels = op_channel_count(file.get(), -1);
    const ogg_int64_t total = op_pcm_total(file.get(), -1);
    if (channels < 1 || channels > 2 || total <= 0 || total > qint64(MaxSeconds + 5) * OpusRate)
        return -1;
    return qint64(total) * 1000 / OpusRate;
}

std::optional<Audio> fromOpus(const QByteArray &ogg)
{
    if (ogg.size() < 4 || !ogg.startsWith("OggS") || !cleanPages(ogg))
        return std::nullopt;
    int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> file(
        op_open_memory(reinterpret_cast<const unsigned char *>(ogg.constData()), size_t(ogg.size()), &error), op_free);
    if (!file || error != 0)
        return std::nullopt;
    // One stream (a chained file could change its channels on the way), mono
    // or stereo, not longer than a recording can be.
    const int channels = op_channel_count(file.get(), -1);
    const qint64 maxFrames = qint64(MaxSeconds + 5) * OpusRate;
    const ogg_int64_t total = op_pcm_total(file.get(), -1);
    if (op_link_count(file.get()) != 1 || channels < 1 || channels > 2 || total < 0 || total > maxFrames)
        return std::nullopt;

    Audio audio;
    audio.sampleRate = OpusRate;
    audio.channels = channels;
    audio.samples.reserve(qsizetype(total) * channels * 2);
    // 120 ms at 48 kHz is the most one read gives, per channel.
    opus_int16 buffer[5760 * 2];
    qint64 frames = 0;
    for (;;) {
        int link = -1;
        const int read = op_read(file.get(), buffer, int(std::size(buffer)), &link);
        if (read == 0)
            break;
        // A hole or a broken packet: not a file made by us or any honest app.
        if (read < 0 || link != 0)
            return std::nullopt;
        frames += read;
        if (frames > maxFrames)
            return std::nullopt;
        audio.samples.append(reinterpret_cast<const char *>(buffer), qsizetype(read) * channels * 2);
    }
    if (frames == 0 || frames != total)
        return std::nullopt;
    return audio;
}

} // namespace Voice
