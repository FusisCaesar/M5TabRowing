#pragma once

#include <stdint.h>

// Coaching text for the Efficiency and Stroke screens. The UI font is plain
// ASCII, so a translation leaves accents out. Another language is a value in
// Lang and one more column in the table in i18n.cpp.

enum class Lang : uint8_t { En = 0, Nl = 1, Count };

namespace i18n {

enum Id : uint16_t {
    EffMetres,
    EffYourBest,
    EffEqualSplit,
    EffRateAxis,
    EffTitle,
    EffOfBest,
    EffLearningRate,
    EffVsBest,
    EffRecDrive,
    EffAim,
    AdviceSlower,
    AdviceGood,
    AdviceQuicker,
    EffCheck,
    EffBounce,
    EffUsualMg,
    HintStart,
    HintSlower,
    HintQuickerHands,
    HintSmoother,
    HintNoGps,
    HintLearningBest,
    HintLengthen,
    HintOnBest,
    HintClose,
    StrokeOne,
    StrokeMany,
    StrokeTitle,
    StrokeWaitPhases,
    PhaseCatch,
    PhaseDrive,
    PhaseFinish,
    PhaseRun,
    PhaseApproach,
    PhaseTogether,
    LegGps,
    LegRef,
    LegNow,
    LegFinish,
    LegCatch,
    StrokeWaiting,
    StrokeLearning,
    StrokeVsBest,
    StrokeVsUsual,
    StrokeAverage,
    StrokeNone,
    CapCatch,
    CapDrive,
    CapFinish,
    CapRun,
    CapApproach,
    CapNotch,
    CapNotches,
    WhoBest,
    WhoUsual,
    WhoSharp,
    HintLearnStroke,
    HintCatchTogether,
    HintCatchNotches,
    HintQuickerWater,
    HintFinishTogether,
    HintSharperFinish,
    HintDipLater,
    HintQuickerCatch,
    HintPush,
    HintBuild,
    HintHold,
    HintCleaner,
    HintMatches,
    HintStartStroke,
    HintReading,
    CrewButton,
    CrewOf,
    HoldClearEff,
    StrokeRefCleared,
    LangName,
    Count
};

void set(Lang lang);
Lang current();
const char* code();
const char* tr(Id id);

}  // namespace i18n
