#include "i18n.h"

namespace i18n {

namespace {

Lang lang = Lang::En;

// One row per Id, one column per Lang. A blank cell falls back to English.
const char* const TABLE[][static_cast<int>(Lang::Count)] = {
    {"METRES PER STROKE", "METERS PER SLAG"},
    {"your best", "beste"},
    {"equal split", "gelijke split"},
    {"stroke rate (spm)", "slagtempo (spm)"},
    {"EFFICIENCY", "EFFICIENTIE"},
    {"of your best %.1f m at %d spm", "van je beste %.1f m bij %d spm"},
    {"learning your best at %d spm", "je beste bij %d spm leren"},
    {"distance per stroke vs your best", "meters per slag tegen je beste"},
    {"REC:DRIVE", "OP:HAAL"},
    {"%.1f  aim %.1f", "%.1f  doel %.1f"},
    {"SLOWER", "LANGZAMER"},
    {"GOOD", "GOED"},
    {"QUICKER", "SNELLER"},
    {"CHECK", "CHECK"},
    {"BOUNCE", "STUITER"},
    {"%.0f mg  usual %.0f", "%.0f mg  normaal %.0f"},
    {"Start rowing: the chart fills in stroke by stroke", "Begin te roeien, de grafiek vult zich per slag"},
    {"Slower slide: ratio %.1f, aim for %.1f at %d spm", "Langzamer oprijden: %.1f, doel %.1f bij %d spm"},
    {"Quicker hands away: ratio %.1f, aim for %.1f at %d spm", "Handen sneller weg: %.1f, doel %.1f bij %d spm"},
    {"Smoother slide: check %.0f mg, your usual is %.0f mg", "Rustiger oprijden: check %.0f mg, normaal %.0f"},
    {"No GPS: distance per stroke needs a GPS fix", "Geen GPS: meters per slag heeft een fix nodig"},
    {"Learning your best at %d spm: keep rowing, try other rates too", "Je beste bij %d spm leren: blijf roeien"},
    {"Lengthen and push: %d%% of your best at %d spm (%.1f m)", "Langer en harder: %d%% van je beste bij %d spm (%.1f m)"},
    {"On your best line at %d spm - hold this stroke", "Op je beste lijn bij %d spm, houd deze slag"},
    {"Close to your best at %d spm: %d%%", "Dicht bij je beste bij %d spm: %d%%"},
    {"%d stroke", "%d slag"},
    {"%d strokes", "%d slagen"},
    {"STROKE", "SLAG"},
    {"The phases appear after a few strokes", "De fasen komen na een paar slagen"},
    {"CATCH", "INPIK"},
    {"DRIVE", "HAAL"},
    {"FINISH", "UITPIK"},
    {"RUN", "UITLOOP"},
    {"APPROACH", "AANLOOP"},
    {"TOGETHER", "SAMEN"},
    {"gps", "gps"},
    {"reference", "referentie"},
    {"now", "nu"},
    {"finish", "uitpik"},
    {"catch", "inpik"},
    {"waiting", "wachten"},
    {"learning", "leren"},
    {"vs your best", "tegen je beste"},
    {"vs your usual", "tegen je normale"},
    {"%d-stroke average", "gemiddelde %d slagen"},
    {"no stroke yet", "nog geen slag"},
    {"%.2f g deep, loses %.2f m/s", "%.2f g diep, verliest %.2f m/s"},
    {"peak %d%%, sag %.2f g", "piek %d%%, dal %.2f g"},
    {"release dip %.2f g", "uitpikdip %.2f g"},
    {"speed peaks at %.1f s", "snelheid piekt na %.1f s"},
    {"loses %.2f m/s before the blade is in", "verliest %.2f m/s voor de inpik"},
    {"1 notch in the catch", "1 knik in de inpik"},
    {"%.0f notches in the catch", "%.0f knikken in de inpik"},
    {"best", "beste"},
    {"usual", "normale"},
    {"sharpest", "scherpste"},
    {"Learning your stroke at %d spm", "Je slag bij %d spm leren"},
    {"Catch together: catch %d ms wide, your %s %d ms", "Samen inpikken: %d ms breed, je %s %d ms"},
    {"Catch together: %.0f notches, your %s has %.0f", "Samen inpikken: %.0f knikken, je %s %.0f"},
    {"Quicker into the water: catch %d ms wide, your %s %d ms", "Sneller inpikken: %d ms breed, je %s %d ms"},
    {"Finish together: release takes %d ms, your %s %d ms", "Samen uitpikken: %d ms, je %s %d ms"},
    {"Sharper finish: release takes %d ms, your %s %d ms", "Scherpere uitpik: %d ms, je %s %d ms"},
    {"Dip later: slows %.2f s before the catch, your %s %.2f s", "Later inpikken: %.2f s voor de inpik, je %s %.2f s"},
    {"Quicker catch: losing %.2f m/s, your %s %.2f", "Snellere inpik: verliest %.2f m/s, je %s %.2f"},
    {"Push harder: drive adds %.2f m/s, your %s %.2f at %d spm", "Harder drukken: haal %.2f m/s, je %s %.2f bij %d spm"},
    {"Build the drive earlier: peak at %d%% of the drive", "Eerder opbouwen: piek op %d%% van de haal"},
    {"Hold it through the finish: peak at %d%% of the drive", "Vasthouden tot de uitpik: piek op %d%%"},
    {"Cleaner release: dip %.2f g, your %s %.2f g", "Schoner uitpikken: dip %.2f g, je %s %.2f g"},
    {"Matches your %s stroke at %d spm", "Gelijk aan je %s slag bij %d spm"},
    {"Start rowing: the stroke builds up catch by catch", "Begin te roeien, de slag bouwt zich op"},
    {"Reading the stroke shape", "Slagvorm wordt gelezen"},
    {"CREW %d", "PLOEG %d"},
    {"Crew of %d", "Ploeg van %d"},
    {"Hold to clear the chart and your best line", "Vasthouden wist de grafiek"},
    {"Stroke reference cleared", "Slagreferentie gewist"},
    {"English", "Nederlands"},
};

static_assert(sizeof(TABLE) / sizeof(TABLE[0]) == static_cast<int>(Id::Count), "locale rows");

}  // namespace

void set(Lang next) {
    if (static_cast<uint8_t>(next) >= static_cast<uint8_t>(Lang::Count)) next = Lang::En;
    lang = next;
}

Lang current() { return lang; }

const char* code() {
    switch (lang) {
        case Lang::Nl: return "NL";
        default: return "EN";
    }
}

const char* tr(Id id) {
    int row = static_cast<int>(id);
    if (row < 0 || row >= static_cast<int>(Id::Count)) return "";
    const char* s = TABLE[row][static_cast<int>(lang)];
    if (!s || !s[0]) s = TABLE[row][static_cast<int>(Lang::En)];
    return s ? s : "";
}

}  // namespace i18n
