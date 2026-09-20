#include "engine/cachingreader/stemcachingreader.h"

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "moc_stemcachingreader.cpp"
#include "track/stemset.h"
#include "track/track.h"
#include "engine/engine.h"
#include "util/logger.h"
#include "util/math.h"
#include "util/sample.h"

namespace {

const mixxx::Logger kLogger("StemCachingReader");

// Stem order, matching StemControls and the skin panel.
constexpr int kVocals = 0;
constexpr int kInstrumental = 1;

const QString kStemItems[2] = {
        QStringLiteral("stem_vocals_enabled"),
        QStringLiteral("stem_instrumental_enabled"),
};
const QString kStemAvailable = QStringLiteral("stem_available");

// How fast a stem fades in or out when it is toggled. Long enough that the
// step is not heard as a click, short enough that the DJ hears the drop on the
// beat they asked for: ~5 ms at 48 kHz.
constexpr SINT kRampFrames = 256;

} // anonymous namespace

StemCachingReader::StemCachingReader(const QString& group, UserSettingsPointer pConfig)
        : CachingReader(group, pConfig),
          m_group(group),
          m_pConfig(pConfig),
          m_stemsActive(false),
          m_pScheduler(nullptr) {
    for (int i = 0; i < 2; ++i) {
        auto pButton = std::make_unique<ControlPushButton>(
                ConfigKey(group, kStemItems[i]), /*bPersist*/ false, /*defaultValue*/ 1.0);
        pButton->setButtonMode(ControlPushButton::TOGGLE);
        pButton->set(1.0);
        m_pStemEnabled[i] = std::move(pButton);
        m_gain[i] = 1.0f;
    }
    m_pStemAvailable = std::make_unique<ControlObject>(ConfigKey(group, kStemAvailable));
}

StemCachingReader::~StemCachingReader() = default;

double StemCachingReader::targetGain(int stemIndex) const {
    return m_pStemEnabled[stemIndex]->toBool() ? 1.0 : 0.0;
}

void StemCachingReader::newTrack(TrackPointer pTrack) {
    const StemSet stems = pTrack
            ? StemSet::forTrackLocation(pTrack->getLocation())
            : StemSet();
    if (pTrack && !stems.rejection().isEmpty()) {
        kLogger.info() << m_group << "ignoring stems beside" << pTrack->getLocation()
                       << ':' << stems.rejection();
    }

    m_pDeckTrack = pTrack;
    m_stemsActive = stems.isValid();
    m_pStemAvailable->forceSet(m_stemsActive ? 1.0 : 0.0);
    if (!m_stemsActive) {
        CachingReader::newTrack(pTrack);
        if (m_pInstrumental) {
            m_pInstrumental->newTrack(TrackPointer());
        }
        return;
    }

    if (!m_pInstrumental) {
        // First stems track on this deck: the second reader (5 MB and a worker
        // thread) is not paid for until it is needed.
        m_pInstrumental = std::make_unique<CachingReader>(
                m_group + QStringLiteral("_stem"), m_pConfig);
        m_pInstrumental->setScheduler(m_pScheduler);
    }
    // Temporary tracks: these files are audio for this deck, not library
    // entries, and nothing should analyze, count or remember them.
    CachingReader::newTrack(Track::newTemporary(stems.vocalsPath()));
    m_pInstrumental->newTrack(Track::newTemporary(stems.instrumentalPath()));
    kLogger.info() << m_group << "playing stems for" << pTrack->getLocation();
}

void StemCachingReader::onWorkerTrackLoaded(TrackPointer pTrack,
        mixxx::audio::SampleRate trackSampleRate,
        double trackNumSamples) {
    // The stems are the same length and rate as the track they came from (the
    // separation pass renders them from one decode of it), so the deck's
    // position, length and waveform still line up with the audio.
    CachingReader::onWorkerTrackLoaded(m_stemsActive && m_pDeckTrack ? m_pDeckTrack : pTrack,
            trackSampleRate,
            trackNumSamples);
}

void StemCachingReader::process() {
    CachingReader::process();
    if (m_pInstrumental) {
        m_pInstrumental->process();
    }
}

void StemCachingReader::hintAndMaybeWake(const HintVector& hintList) {
    CachingReader::hintAndMaybeWake(hintList);
    if (m_stemsActive && m_pInstrumental) {
        // The same hints: both stems are read at the same positions, always.
        m_pInstrumental->hintAndMaybeWake(hintList);
    }
}

void StemCachingReader::setScheduler(EngineWorkerScheduler* pScheduler) {
    m_pScheduler = pScheduler;
    CachingReader::setScheduler(pScheduler);
    if (m_pInstrumental) {
        m_pInstrumental->setScheduler(pScheduler);
    }
}

void StemCachingReader::mixStems(CSAMPLE* pVocals,
        CSAMPLE* pInstrumental,
        SINT numSamples,
        CSAMPLE* pGains,
        const CSAMPLE* pTargets) {
    // Both stems are scaled, so "both on" is the track exactly as recorded.
    for (int stem = 0; stem < 2; ++stem) {
        CSAMPLE* pStem = stem == kVocals ? pVocals : pInstrumental;
        if (pGains[stem] == pTargets[stem]) {
            SampleUtil::applyGain(pStem, pGains[stem], numSamples);
            continue;
        }
        const SINT rampSamples =
                math_min(numSamples, kRampFrames * mixxx::kEngineChannelCount);
        SampleUtil::applyRampingGain(pStem, pGains[stem], pTargets[stem], rampSamples);
        if (numSamples > rampSamples) {
            SampleUtil::applyGain(pStem + rampSamples, pTargets[stem], numSamples - rampSamples);
        }
        pGains[stem] = pTargets[stem];
    }
    SampleUtil::addWithGain(pVocals, pInstrumental, 1.0f, numSamples);
}

CachingReader::ReadResult StemCachingReader::read(
        SINT startSample, SINT numSamples, bool reverse, CSAMPLE* buffer) {
    if (!m_stemsActive || !m_pInstrumental) {
        return CachingReader::read(startSample, numSamples, reverse, buffer);
    }

    const ReadResult vocalsResult = CachingReader::read(startSample, numSamples, reverse, buffer);
    if (vocalsResult == ReadResult::UNAVAILABLE) {
        // Nothing was written to the buffer; do not mix into it.
        return vocalsResult;
    }
    if (static_cast<SINT>(m_mixBuffer.size()) < numSamples) {
        m_mixBuffer.resize(numSamples);
    }
    const ReadResult otherResult =
            m_pInstrumental->read(startSample, numSamples, reverse, m_mixBuffer.data());
    if (otherResult == ReadResult::UNAVAILABLE) {
        // The instrumental has not caught up. Silence is the honest answer for
        // its half rather than letting the vocal through at full level.
        SampleUtil::clear(m_mixBuffer.data(), numSamples);
    }

    const CSAMPLE targets[2] = {static_cast<CSAMPLE>(targetGain(kVocals)),
            static_cast<CSAMPLE>(targetGain(kInstrumental))};
    mixStems(buffer, m_mixBuffer.data(), numSamples, m_gain, targets);

    return vocalsResult == ReadResult::AVAILABLE && otherResult == ReadResult::AVAILABLE
            ? ReadResult::AVAILABLE
            : ReadResult::PARTIALLY_AVAILABLE;
}
