#include "mixer/stemcontrols.h"

#include "control/controlobject.h"
#include "control/controlpushbutton.h"

namespace {

// Names the skin and controller mappings bind to. Named rather than numbered
// because two stems are the decision, not a step towards four, and
// "stem_vocals_enabled" says what a pad does where "stem_1_enabled" does not.
const QString kStemItems[StemControls::kStemCount] = {
        QStringLiteral("stem_vocals_enabled"),
        QStringLiteral("stem_instrumental_enabled"),
};

} // anonymous namespace

StemControls::StemControls(const QString& group) {
    m_pAvailable = std::make_unique<ControlObject>(
            ConfigKey(group, QStringLiteral("stem_available")));
    m_pAvailable->setReadOnly();

    for (int i = 0; i < kStemCount; ++i) {
        auto pButton = std::make_unique<ControlPushButton>(
                ConfigKey(group, kStemItems[i]), /*bPersist*/ false, /*defaultValue*/ 1.0);
        pButton->setButtonMode(ControlPushButton::TOGGLE);
        pButton->set(1.0);
        m_pStemEnabled[i] = std::move(pButton);
    }
}

StemControls::~StemControls() = default;

void StemControls::setAvailable(bool available) {
    m_pAvailable->forceSet(available ? 1.0 : 0.0);
}

bool StemControls::isEnabled(int stemIndex) const {
    if (stemIndex < 0 || stemIndex >= kStemCount) {
        return false;
    }
    return m_pStemEnabled[stemIndex]->toBool();
}
