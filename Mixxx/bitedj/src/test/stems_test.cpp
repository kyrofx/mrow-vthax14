// Tests for Bite DJ stems: deciding whether a track's stems can be trusted,
// and summing them into the deck's audio.
//
// What is NOT covered here is decoding: that needs a real reader, its worker
// thread and an audio file, and the thing worth proving about it — that two
// Opus streams fit the Pi's budget — was measured on the device instead
// (RPI/bitedj_docs/stems.md).
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "engine/cachingreader/stemcachingreader.h"
#include "track/stemset.h"
#include "util/sample.h"

namespace {

constexpr int kManifestVersion = 1;

class StemSetTest : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_track = m_dir.filePath(QStringLiteral("song.mp3"));
        writeFile(m_track, QByteArray(4096, 'x'));
        m_stems = m_track + QStringLiteral(".stems");
        ASSERT_TRUE(QDir().mkpath(m_stems));
        writeFile(stemPath(QStringLiteral("vocals.opus")), QByteArray("OggS"));
        writeFile(stemPath(QStringLiteral("instrumental.opus")), QByteArray("OggS"));
        writeManifest(manifest());
    }

    static void writeFile(const QString& path, const QByteArray& bytes) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

    QString stemPath(const QString& name) const {
        return m_stems + QLatin1Char('/') + name;
    }

    /// The manifest separate-stems.py writes for this track.
    QJsonObject manifest() const {
        QJsonObject source{{"name", QStringLiteral("song.mp3")}, {"bytes", 4096}};
        QJsonObject stems{{"vocals", QStringLiteral("vocals.opus")},
                {"instrumental", QStringLiteral("instrumental.opus")}};
        return QJsonObject{{"version", kManifestVersion},
                {"model", QStringLiteral("htdemucs")},
                {"source", source},
                {"stems", stems}};
    }

    void writeManifest(const QJsonObject& object) {
        writeFile(stemPath(QStringLiteral("manifest.json")),
                QJsonDocument(object).toJson());
    }

    QTemporaryDir m_dir;
    QString m_track;
    QString m_stems;
};

TEST_F(StemSetTest, AcceptsStemsThatBelongToTheTrack) {
    const StemSet stems = StemSet::forTrackLocation(m_track);
    ASSERT_TRUE(stems.isValid());
    EXPECT_EQ(stemPath(QStringLiteral("vocals.opus")), stems.vocalsPath());
    EXPECT_EQ(stemPath(QStringLiteral("instrumental.opus")), stems.instrumentalPath());
    EXPECT_TRUE(stems.rejection().isEmpty());
}

TEST_F(StemSetTest, ATrackWithoutStemsIsNotAnError) {
    const QString plain = m_dir.filePath(QStringLiteral("other.mp3"));
    writeFile(plain, QByteArray(128, 'y'));
    const StemSet stems = StemSet::forTrackLocation(plain);
    EXPECT_FALSE(stems.isValid());
    // Nothing to report: most of a library is not separated.
    EXPECT_TRUE(stems.rejection().isEmpty());
}

TEST_F(StemSetTest, RejectsStemsLeftBehindByADifferentFile) {
    // The case that matters: the track was re-encoded or replaced under the
    // same name, and its old stems are still sitting beside it. Playing those
    // would be playing another song's vocals over this one.
    writeFile(m_track, QByteArray(9000, 'z'));
    const StemSet stems = StemSet::forTrackLocation(m_track);
    EXPECT_FALSE(stems.isValid());
    EXPECT_EQ(QStringLiteral("stems belong to a different file"), stems.rejection());
}

TEST_F(StemSetTest, RejectsAManifestFromANewerSeparationPass) {
    QJsonObject newer = manifest();
    newer.insert(QStringLiteral("version"), kManifestVersion + 1);
    writeManifest(newer);
    const StemSet stems = StemSet::forTrackLocation(m_track);
    EXPECT_FALSE(stems.isValid());
    EXPECT_TRUE(stems.rejection().contains(QStringLiteral("version")));
}

TEST_F(StemSetTest, RejectsAnIncompleteSet) {
    ASSERT_TRUE(QFile::remove(stemPath(QStringLiteral("instrumental.opus"))));
    const StemSet stems = StemSet::forTrackLocation(m_track);
    EXPECT_FALSE(stems.isValid());
    EXPECT_EQ(QStringLiteral("a stem file is missing"), stems.rejection());
}

TEST_F(StemSetTest, RejectsRubbishInsteadOfAManifest) {
    writeFile(stemPath(QStringLiteral("manifest.json")), QByteArray("not json"));
    EXPECT_FALSE(StemSet::forTrackLocation(m_track).isValid());
}

TEST_F(StemSetTest, FollowsTheStemNamesTheManifestGives) {
    // The names are data, so a later pass can change format without the
    // device guessing at extensions.
    writeFile(stemPath(QStringLiteral("v.flac")), QByteArray("fLaC"));
    writeFile(stemPath(QStringLiteral("i.flac")), QByteArray("fLaC"));
    QJsonObject renamed = manifest();
    renamed.insert(QStringLiteral("stems"),
            QJsonObject{{"vocals", QStringLiteral("v.flac")},
                    {"instrumental", QStringLiteral("i.flac")}});
    writeManifest(renamed);
    const StemSet stems = StemSet::forTrackLocation(m_track);
    ASSERT_TRUE(stems.isValid());
    EXPECT_EQ(stemPath(QStringLiteral("v.flac")), stems.vocalsPath());
}

// ---- mixing ----------------------------------------------------------------

class StemMixTest : public testing::Test {
  protected:
    static constexpr SINT kSamples = 2048;

    void SetUp() override {
        for (SINT i = 0; i < kSamples; ++i) {
            m_vocals[i] = 1.0f;
            m_instrumental[i] = 0.25f;
        }
    }

    /// The tail of the buffer, past any ramp, is the steady state.
    CSAMPLE settled() const {
        return m_vocals[kSamples - 1];
    }

    CSAMPLE m_vocals[kSamples];
    CSAMPLE m_instrumental[kSamples];
    CSAMPLE m_gains[2] = {1.0f, 1.0f};
};

TEST_F(StemMixTest, BothStemsOnIsTheTrackAsRecorded) {
    const CSAMPLE targets[2] = {1.0f, 1.0f};
    StemCachingReader::mixStems(m_vocals, m_instrumental, kSamples, m_gains, targets);
    // No gain applied to either: the sum is exactly what was separated.
    EXPECT_FLOAT_EQ(1.25f, settled());
    EXPECT_FLOAT_EQ(1.25f, m_vocals[0]);
}

TEST_F(StemMixTest, DroppingVocalsLeavesTheInstrumental) {
    const CSAMPLE targets[2] = {0.0f, 1.0f};
    StemCachingReader::mixStems(m_vocals, m_instrumental, kSamples, m_gains, targets);
    EXPECT_FLOAT_EQ(0.25f, settled());
    EXPECT_FLOAT_EQ(0.0f, m_gains[0]);
    EXPECT_FLOAT_EQ(1.0f, m_gains[1]);
}

TEST_F(StemMixTest, DroppingTheInstrumentalLeavesTheVocals) {
    const CSAMPLE targets[2] = {1.0f, 0.0f};
    StemCachingReader::mixStems(m_vocals, m_instrumental, kSamples, m_gains, targets);
    EXPECT_FLOAT_EQ(1.0f, settled());
}

TEST_F(StemMixTest, ATogglePassesThroughTheValuesBetweenInsteadOfStepping) {
    // A hard gate on a stem is audible as a click, so the gain ramps. The
    // first sample must still be near where the last buffer left off.
    const CSAMPLE targets[2] = {0.0f, 0.0f};
    StemCachingReader::mixStems(m_vocals, m_instrumental, kSamples, m_gains, targets);
    EXPECT_GT(m_vocals[0], 1.0f);
    EXPECT_FLOAT_EQ(0.0f, settled());
    bool sawIntermediate = false;
    for (SINT i = 0; i < kSamples; ++i) {
        if (m_vocals[i] > 0.01f && m_vocals[i] < 1.24f) {
            sawIntermediate = true;
            break;
        }
    }
    EXPECT_TRUE(sawIntermediate);
}

TEST_F(StemMixTest, GainsAreHeldAcrossBuffers) {
    const CSAMPLE targets[2] = {0.0f, 1.0f};
    StemCachingReader::mixStems(m_vocals, m_instrumental, kSamples, m_gains, targets);
    // Second buffer: no ramp left to do, so the whole buffer is steady.
    SetUp();
    StemCachingReader::mixStems(m_vocals, m_instrumental, kSamples, m_gains, targets);
    EXPECT_FLOAT_EQ(0.25f, m_vocals[0]);
    EXPECT_FLOAT_EQ(0.25f, settled());
}

TEST_F(StemMixTest, ARampShorterThanTheBufferStillFinishes) {
    // Buffers can be smaller than the ramp; the gain must not stall partway.
    const CSAMPLE targets[2] = {0.0f, 1.0f};
    StemCachingReader::mixStems(m_vocals, m_instrumental, 64, m_gains, targets);
    EXPECT_FLOAT_EQ(0.0f, m_gains[0]);
}

} // namespace
