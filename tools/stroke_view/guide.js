"use strict";

// The Guide page: what the recording contains and what you can conclude from it. Four parts: where the
// numbers come from, the anatomy of one stroke (a drawn diagram), a real stroke from the demo recording
// with numbered marks, and what each figure of the whole outing lets you deduce. The words are here, in
// English and Dutch; the numbers in the real stroke come from /api/guide.

const GUIDE_CHROME = {
  en: { pageStart: "Start", pageGuide: "Guide", welcomeGuide: "How to read your data" },
  nl: { pageStart: "Start", pageGuide: "Uitleg", welcomeGuide: "Zo lees je je data" },
};
Object.assign(CHROME.en, GUIDE_CHROME.en);
Object.assign(CHROME.nl, GUIDE_CHROME.nl);
applyChrome();

const GUIDE = {
  en: {
    title: "How to read your data",
    lead: "The tablet in the boat records how the boat moves, a hundred times a second. This page shows what is measured, what one stroke looks like in that data, and what you can and cannot conclude from it.",
    sources: {
      title: "Where the numbers come from",
      lead: "Each sensor tells a different part of the story.",
      measures: "Measures",
      gives: "Gives you",
      limit: "Keep in mind",
      items: [
        {
          key: "gps", name: "GPS",
          measures: "Position and speed, once a second.",
          gives: ["Speed and split (time per 500 m)", "Distance and the track on the map", "Distance per stroke"],
          limit: "Good for how fast you go, not for what happens inside one stroke.",
        },
        {
          key: "accel", name: "Accelerometer",
          measures: "How the boat speeds up and slows down, a hundred times a second.",
          gives: ["Every stroke found, and the stroke rate", "The surge curve: the shape of a stroke", "The sideways swing"],
          limit: "It shows changes in speed, not speed itself. The speed is rebuilt from it and pinned to the GPS.",
        },
        {
          key: "gyro", name: "Gyro",
          measures: "How fast the boat turns, a hundred times a second.",
          gives: ["Roll: the boat tipping towards one side", "Yaw: the bow swinging left and right"],
          limit: "Movement around the boat's own average, not a compass heading.",
        },
        {
          key: "hr", name: "Heart rate monitor",
          measures: "Beats per minute, once a second, when one is connected.",
          gives: ["Heart rate over the whole outing"],
          limit: "Needs a sensor. Without one the session simply shows no heart rate.",
        },
      ],
    },
    cycle: {
      title: "Anatomy of a stroke",
      lead: "Every stroke leaves the same signature in the boat's acceleration. The top line is acceleration: above zero the boat is speeding up, below zero it is slowing down. The bottom line is the boat speed that follows from it. Point at a phase to read what it means.",
      accel: "Acceleration",
      speed: "Boat speed",
      time: "Time within one stroke, in seconds (the catch is at 0)",
      happens: "What happens",
      measure: "What we measure",
      look: "What to look for",
      aria: "One stroke drawn as acceleration and boat speed, split into catch, drive, finish, run and approach.",
      phases: [
        {
          happens: "The blade goes in and the boat is held back for a moment: the acceleration dips below zero.",
          measure: "Catch width (how long the dip lasts), depth, notches (separate dips) and the speed lost.",
          look: "A short, clean dip is a sharp catch. A wide or double dip means the blade went in late or was not loaded at once.",
        },
        {
          happens: "Legs, back and arms push the boat forward: the acceleration rises to a peak and falls again.",
          measure: "Drive peak (where in the drive it falls), the speed gained, and sag (a valley between two humps).",
          look: "One smooth hump is what you are after. A sag halfway means the pressure let up.",
        },
        {
          happens: "The blade comes out of the water. The acceleration drops from its peak to zero.",
          measure: "Finish time (from 80% to 20% of the peak) and the release dip (how far the acceleration falls below the run just after).",
          look: "A quick, clean finish. A long fall or a deep dip means the blade drags, or the boat is checked as it leaves the water.",
        },
        {
          happens: "The boat glides while you come up the slide.",
          measure: "How uneven the glide is, in milli-g, and when the boat speed peaks.",
          look: "A steady glide. A jerky run shows body or seat movements that tug at the boat.",
        },
        {
          happens: "As you come forward the boat slows down before the blade is in again.",
          measure: "Approach lead (how early the slowing starts) and the speed lost before the catch.",
          look: "Slowing early or a lot usually points to a rushed slide.",
        },
      ],
    },
    example: {
      title: "A real stroke",
      lead: (d, nf) => "One stroke from the demo recording, at " + nf(d.rate, 1) + " strokes a minute and " + nf(d.dps, 1) +
        " m per stroke. The numbers mark what the Strokes page measures. Point at one in the list to find it on the curve.",
      loading: "Loading the example…",
      missing: "The example recording is not available here.",
      aria: "A real stroke from the demo recording with numbered marks.",
      accel: "Acceleration (g)",
      speed: "Boat speed (m/s)",
      marks: [
        (d, nf) => ({
          title: "Catch",
          text: "The blade goes in and the boat is held back for a moment. Here the dip is " + nf(d.now.depth, 2) + " g deep and " +
            nf(d.now.width * 1000, 0) + " ms wide, in " + (d.now.notches < 1.5 ? "one dip" : nf(d.now.notches, 0) + " separate dips (notches)") +
            ". The narrower and cleaner it is, the quicker the blade took hold.",
        }),
        (d, nf) => ({
          title: "Drive peak",
          text: "The hardest push of the stroke, " + nf(d.now.peak, 2) + " g, " + nf(d.now.peak_pct * 100, 0) +
            "% of the way through the drive. One smooth hump is what you are after.",
        }),
        (d, nf) => ({
          title: "Sag",
          text: "A valley between two humps in the drive, " + nf(d.now.sag, 2) + " g deep. It shows the push let up halfway and picked up again.",
        }),
        (d, nf) => ({
          title: "Finish",
          text: "The push dies away in " + nf(d.now.fall * 1000, 0) + " ms, measured from 80% to 20% of the peak. Quick and clean means the blade leaves the water without dragging.",
        }),
        (d, nf) => ({
          title: "Release dip",
          text: "Just after the blade is out, the acceleration falls " + nf(d.now.release, 2) + " g below the glide: the boat is checked a little as the blade lets go.",
        }),
        (d, nf) => ({
          title: "Speed peak",
          text: "The boat speed, rebuilt from the acceleration, is highest " + nf(d.now.speed_peak, 1) + " s after the catch. From there it only slows until the next stroke.",
        }),
      ],
    },
    outing: {
      title: "Reading the whole outing",
      lead: "The Overview and the By rate chart turn all those strokes into a few figures. Here is what each lets you conclude, and where to be careful.",
      see: "You see",
      deduce: "You can deduce",
      cannot: "It cannot tell you",
      axes: {
        title: "How the boat moves",
        aria: "The boat from above with the forward and sideways directions and the yaw, and from behind with the roll.",
        top: "From above",
        rear: "From behind",
        bow: "bow",
        surge: "Surge",
        sway: "Sideways",
        yaw: "Yaw",
        roll: "Roll",
        notes: [
          ["surge", "Forward: the push along the boat. This is the stroke curve above."],
          ["sway", "Left and right across the boat: the sideways swing."],
          ["yaw", "The bow swinging left and right."],
          ["roll", "The boat tipping towards port or starboard."],
        ],
      },
      cards: [
        {
          tag: "Overview", title: "Track and colours",
          see: "Your track on the map, red where you were slow and green where you were fast.",
          deduce: "Where you sped up, slowed down, turned or stopped, and how the pace changed along the course.",
          cannot: "Why you were slow. Wind, current and effort look the same on the map.",
        },
        {
          tag: "Overview", title: "Split and stroke rate",
          see: "Time per 500 m and strokes per minute, second by second.",
          deduce: "How even your pace was, and whether a faster split came from a higher rate or from more boat speed per stroke.",
          cannot: "Split is worked out from the GPS speed, so it lags a few seconds and includes any current.",
        },
        {
          tag: "Overview", title: "Distance per stroke",
          see: "The metres the boat travels for each stroke.",
          deduce: "How much run each stroke gives. The same speed with fewer strokes is more efficient rowing.",
          cannot: "Rate, wind and current all change it, so compare strokes at the same rate (see By rate).",
        },
        {
          tag: "Overview", title: "Heart rate",
          see: "Beats per minute over the outing, with the average and the maximum.",
          deduce: "How hard you worked, and how quickly you settled after a hard piece.",
          cannot: "It trails the effort by tens of seconds and differs from day to day: a guide, not a measure of power.",
        },
        {
          tag: "Overview · Strokes", title: "Roll",
          see: "How much the boat tips from side to side, in degrees (rms).",
          deduce: "Balance. More rocking than usual points to uneven handle heights or an unsteady set. A pattern that repeats every stroke is technique; an irregular one is mostly water and wind.",
          cannot: "Roll is measured around the boat's own average, so a steady list to one side does not show.",
        },
        {
          tag: "Overview · Strokes", title: "Yaw",
          see: "How much the bow swings left and right, in degrees (rms).",
          deduce: "Steering, and uneven pressure: a boat that yaws in step with the stroke has one side pushing harder than the other.",
          cannot: "It is not a compass heading, so a steady turn or a long curve does not show.",
        },
        {
          tag: "Strokes", title: "Sideways swing",
          see: "The boat's sideways acceleration within each stroke.",
          deduce: "How the boat swings from side to side during a stroke. What repeats stroke after stroke is the grey usual line.",
          cannot: "It is acceleration, not distance. A rolling boat tilts the sensor and also reads as sideways.",
        },
        {
          tag: "Strokes", title: "By rate and spells",
          see: "Your strokes grouped by rate, with the average of the best strokes at each rate.",
          deduce: "Which rate carries the boat furthest per stroke and which pace you can hold. If you row in hard and light spells the page says so instead of naming one pace.",
          cannot: "A rate with only a few strokes gets no reference: it needs enough strokes at the same rate.",
        },
      ],
    },
  },
  nl: {
    title: "Zo lees je je data",
    lead: "De tablet in de boot legt vast hoe de boot beweegt, honderd keer per seconde. Deze pagina laat zien wat er gemeten wordt, hoe één slag eruitziet in die data en wat je er wel en niet uit kunt afleiden.",
    sources: {
      title: "Waar de cijfers vandaan komen",
      lead: "Elke sensor vertelt een ander deel van het verhaal.",
      measures: "Meet",
      gives: "Levert op",
      limit: "Houd in gedachten",
      items: [
        {
          key: "gps", name: "GPS",
          measures: "Positie en snelheid, één keer per seconde.",
          gives: ["Snelheid en split (tijd per 500 m)", "Afstand en het spoor op de kaart", "Afstand per slag"],
          limit: "Goed voor hoe snel je gaat, niet voor wat er binnen één slag gebeurt.",
        },
        {
          key: "accel", name: "Versnellingsmeter",
          measures: "Hoe de boot versnelt en vertraagt, honderd keer per seconde.",
          gives: ["Elke slag gevonden, en het slagtempo", "De surge-curve: de vorm van een slag", "De zijwaartse zwaai"],
          limit: "Hij laat veranderingen in snelheid zien, niet de snelheid zelf. Die wordt eruit herberekend en aan de GPS vastgezet.",
        },
        {
          key: "gyro", name: "Gyroscoop",
          measures: "Hoe snel de boot draait, honderd keer per seconde.",
          gives: ["Rol: de boot die naar één kant kantelt", "Gieren: de boeg die naar links en rechts zwaait"],
          limit: "Beweging rond het eigen gemiddelde van de boot, geen kompaskoers.",
        },
        {
          key: "hr", name: "Hartslagmeter",
          measures: "Slagen per minuut, één keer per seconde, als er een verbonden is.",
          gives: ["Hartslag over de hele sessie"],
          limit: "Heeft een sensor nodig. Zonder sensor toont de sessie geen hartslag.",
        },
      ],
    },
    cycle: {
      title: "Anatomie van een slag",
      lead: "Elke slag laat dezelfde handtekening achter in de versnelling van de boot. De bovenste lijn is de versnelling: boven nul versnelt de boot, onder nul vertraagt hij. De onderste lijn is de bootsnelheid die daaruit volgt. Wijs een fase aan om te lezen wat die betekent.",
      accel: "Versnelling",
      speed: "Bootsnelheid",
      time: "Tijd binnen één slag, in seconden (de inpik is bij 0)",
      happens: "Wat gebeurt er",
      measure: "Wat we meten",
      look: "Waar je op let",
      aria: "Eén slag getekend als versnelling en bootsnelheid, verdeeld in inpik, haal, uitpik, uitloop en aanloop.",
      phases: [
        {
          happens: "De spaan gaat erin en de boot wordt even teruggehouden: de versnelling duikt onder nul.",
          measure: "Inpikbreedte (hoelang de dip duurt), diepte, knikken (aparte dips) en de verloren snelheid.",
          look: "Een korte, schone dip is een scherpe inpik. Een brede of dubbele dip betekent dat de spaan laat of niet in één keer belast werd.",
        },
        {
          happens: "Benen, rug en armen duwen de boot vooruit: de versnelling stijgt naar een piek en zakt weer.",
          measure: "Haalpiek (waar in de haal die valt), de snelheidswinst en het dal (een kuil tussen twee bulten).",
          look: "Eén vloeiende bult is wat je zoekt. Een dal halverwege betekent dat de druk wegviel.",
        },
        {
          happens: "De spaan komt uit het water. De versnelling zakt van de piek naar nul.",
          measure: "Uitpiktijd (van 80% naar 20% van de piek) en de uitpikdip (hoever de versnelling daarna onder de uitloop zakt).",
          look: "Een snelle, schone uitpik. Een lange val of diepe dip betekent dat de spaan sleept, of dat de boot wordt afgeremd als hij loskomt.",
        },
        {
          happens: "De boot glijdt terwijl jij naar voren komt op de rolbank.",
          measure: "Hoe ongelijkmatig de glijfase is, in milli-g, en wanneer de bootsnelheid piekt.",
          look: "Een rustige glijfase. Een schokkerige uitloop laat lichaams- of bankbewegingen zien die aan de boot trekken.",
        },
        {
          happens: "Terwijl je naar voren komt vertraagt de boot voordat de spaan er weer in zit.",
          measure: "Aanloopvoorsprong (hoe vroeg het vertragen begint) en de snelheid die voor de inpik verloren gaat.",
          look: "Vroeg of sterk vertragen wijst meestal op een gehaaste rolbank.",
        },
      ],
    },
    example: {
      title: "Een echte slag",
      lead: (d, nf) => "Eén slag uit de voorbeeldopname, op " + nf(d.rate, 1) + " slagen per minuut en " + nf(d.dps, 1) +
        " m per slag. De nummers markeren wat de pagina Slagen meet. Wijs er een aan in de lijst om hem op de curve te vinden.",
      loading: "Voorbeeld laden…",
      missing: "De voorbeeldopname is hier niet beschikbaar.",
      aria: "Een echte slag uit de voorbeeldopname met genummerde markeringen.",
      accel: "Versnelling (g)",
      speed: "Bootsnelheid (m/s)",
      marks: [
        (d, nf) => ({
          title: "Inpik",
          text: "De spaan gaat erin en de boot wordt even afgeremd. Hier is de dip " + nf(d.now.depth, 2) + " g diep en " +
            nf(d.now.width * 1000, 0) + " ms breed, in " + (d.now.notches < 1.5 ? "één dip" : nf(d.now.notches, 0) + " aparte dips (knikken)") +
            ". Hoe smaller en schoner, hoe sneller de spaan pakte.",
        }),
        (d, nf) => ({
          title: "Haalpiek",
          text: "De hardste duw van de slag, " + nf(d.now.peak, 2) + " g, op " + nf(d.now.peak_pct * 100, 0) +
            "% van de haal. Eén vloeiende bult is wat je zoekt.",
        }),
        (d, nf) => ({
          title: "Dal",
          text: "Een dal tussen twee bulten in de haal, " + nf(d.now.sag, 2) + " g diep. Het laat zien dat de duw halverwege verslapte en weer aantrok.",
        }),
        (d, nf) => ({
          title: "Uitpik",
          text: "De duw dooft uit in " + nf(d.now.fall * 1000, 0) + " ms, gemeten van 80% naar 20% van de piek. Snel en schoon betekent dat de spaan zonder slepen uit het water komt.",
        }),
        (d, nf) => ({
          title: "Uitpikdip",
          text: "Net nadat de spaan eruit is, zakt de versnelling " + nf(d.now.release, 2) + " g onder de uitloop: de boot wordt even afgeremd als de spaan loslaat.",
        }),
        (d, nf) => ({
          title: "Snelheidspiek",
          text: "De bootsnelheid, herberekend uit de versnelling, is het hoogst " + nf(d.now.speed_peak, 1) + " s na de inpik. Daarna vertraagt hij alleen nog tot de volgende slag.",
        }),
      ],
    },
    outing: {
      title: "De hele sessie lezen",
      lead: "Het Overzicht en de grafiek Per slagfrequentie maken van al die slagen een paar cijfers. Dit kun je er telkens uit afleiden, en hier moet je oppassen.",
      see: "Je ziet",
      deduce: "Je kunt afleiden",
      cannot: "Dit zegt het niet",
      axes: {
        title: "Hoe de boot beweegt",
        aria: "De boot van boven met de richtingen vooruit en zijwaarts en het gieren, en van achteren met het rollen.",
        top: "Van boven",
        rear: "Van achteren",
        bow: "boeg",
        surge: "Surge",
        sway: "Zijwaarts",
        yaw: "Gieren",
        roll: "Rol",
        notes: [
          ["surge", "Vooruit: de duw langs de boot. Dit is de slagcurve hierboven."],
          ["sway", "Links en rechts over de boot: de zijwaartse zwaai."],
          ["yaw", "De boeg die naar links en rechts zwaait."],
          ["roll", "De boot die naar bakboord of stuurboord kantelt."],
        ],
      },
      cards: [
        {
          tag: "Overzicht", title: "Spoor en kleuren",
          see: "Je spoor op de kaart, rood waar je langzaam was en groen waar je snel was.",
          deduce: "Waar je versnelde, vertraagde, draaide of stilstond, en hoe het tempo langs het parcours veranderde.",
          cannot: "Waarom je langzaam was. Wind, stroming en inzet zien er op de kaart hetzelfde uit.",
        },
        {
          tag: "Overzicht", title: "Split en slagtempo",
          see: "Tijd per 500 m en slagen per minuut, seconde voor seconde.",
          deduce: "Hoe gelijkmatig je tempo was, en of een snellere split kwam van een hoger tempo of van meer bootsnelheid per slag.",
          cannot: "De split wordt uit de GPS-snelheid berekend, loopt dus een paar seconden achter en bevat eventuele stroming.",
        },
        {
          tag: "Overzicht", title: "Afstand per slag",
          see: "De meters die de boot per slag aflegt.",
          deduce: "Hoeveel uitloop elke slag geeft. Dezelfde snelheid met minder slagen is efficiënter roeien.",
          cannot: "Tempo, wind en stroming veranderen het allemaal, dus vergelijk slagen op hetzelfde tempo (zie Per slagfrequentie).",
        },
        {
          tag: "Overzicht", title: "Hartslag",
          see: "Slagen per minuut over de sessie, met het gemiddelde en het maximum.",
          deduce: "Hoe hard je werkte, en hoe snel je na een zwaar stuk tot rust kwam.",
          cannot: "Hij loopt tientallen seconden achter op de inspanning en verschilt per dag: een richtlijn, geen maat voor vermogen.",
        },
        {
          tag: "Overzicht · Slagen", title: "Rol",
          see: "Hoeveel de boot van links naar rechts kantelt, in graden (rms).",
          deduce: "Balans. Meer schommelen dan normaal wijst op ongelijke handhoogtes of een onrustige set. Een patroon dat elke slag terugkomt is techniek; een onregelmatig patroon is vooral water en wind.",
          cannot: "De rol wordt gemeten rond het eigen gemiddelde van de boot, dus een vaste helling naar één kant zie je niet.",
        },
        {
          tag: "Overzicht · Slagen", title: "Gieren",
          see: "Hoeveel de boeg naar links en rechts zwaait, in graden (rms).",
          deduce: "Sturen, en ongelijke druk: een boot die in de maat van de slag giert, heeft een kant die harder duwt dan de andere.",
          cannot: "Het is geen kompaskoers, dus een vaste draai of een lange bocht zie je niet.",
        },
        {
          tag: "Slagen", title: "Zijwaartse zwaai",
          see: "De zijwaartse versnelling van de boot binnen elke slag.",
          deduce: "Hoe de boot tijdens een slag van links naar rechts zwaait. Wat slag na slag terugkomt is de grijze gebruikelijke lijn.",
          cannot: "Het is versnelling, geen afstand. Een rollende boot kantelt de sensor en leest ook als zijwaarts.",
        },
        {
          tag: "Slagen", title: "Per slagfrequentie en blokken",
          see: "Je slagen gegroepeerd per tempo, met het gemiddelde van de beste slagen op elk tempo.",
          deduce: "Welk tempo de boot per slag het verst brengt en welk tempo je kunt volhouden. Roei je in harde en lichte blokken, dan zegt de pagina dat in plaats van één tempo te noemen.",
          cannot: "Een tempo met maar een paar slagen krijgt geen referentie: er zijn genoeg slagen op hetzelfde tempo nodig.",
        },
      ],
    },
  },
};

const GUIDE_INK = "#1c1915";
const GUIDE_HOT = "#9a3412";
const GUIDE_AXES = { surge: "#1d4e89", sway: "#b86a10", yaw: "#8a6d1d", roll: "#0e7c86" };

const guide = {
  lang: null,
  example: null,
  loading: false,
  hot: 0,
  phase: -1,
};

function guideText() { return GUIDE[lang()]; }

function guideNumber(x, digits) {
  if (Math.abs(x) < 0.5 * Math.pow(10, -digits)) x = 0;  // no "-0.0"
  return x.toLocaleString(lang(), { minimumFractionDigits: digits, maximumFractionDigits: digits, useGrouping: false });
}

// ---- the stroke cycle, drawn from a made-up but physical stroke ----

const CYCLE = { t0: -0.4, t1: 2.0 };
// Catch, drive, finish, run, approach: where each starts, and the approach on both ends of the window.
const CYCLE_BANDS = [
  [-0.4, 0, 4], [0, 0.18, 0], [0.18, 1.0, 1], [1.0, 1.3, 2], [1.3, 1.6, 3], [1.6, 2.0, 4],
];

function cycleCurves() {
  const smooth = (x) => (x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x));
  const bump = (t, m, s) => Math.exp(-0.5 * ((t - m) / s) ** 2);
  const accel = (t) => {
    let a = 0;
    if (t < 0) a += -0.05 - 0.04 * smooth((t + 0.25) / 0.25);
    else a += -0.09 * (1 - smooth(t / 0.1));
    if (t >= 1.6) a += -0.05 * smooth((t - 1.6) / 0.15);
    a += -0.3 * bump(t, 0.02, 0.07);
    a += 0.36 * bump(t, 0.6, 0.26) * (1 - smooth((t - 0.9) / 0.4));
    a += -0.07 * bump(t, 1.25, 0.06);
    a += -0.012 * smooth((t - 1.25) / 0.1) * (1 - smooth((t - 1.55) / 0.1));
    return a;
  };
  const n = 240;
  const dt = (CYCLE.t1 - CYCLE.t0) / n;
  const t = [];
  const a = [];
  for (let i = 0; i <= n; i++) {
    t.push(CYCLE.t0 + i * dt);
    a.push(accel(CYCLE.t0 + i * dt));
  }
  // A steady boat is no faster at the end of a stroke than at its start, so the average acceleration is nil.
  const mean = a.slice(0, n).reduce((s, x) => s + x, 0) / n;
  for (let i = 0; i <= n; i++) a[i] -= mean;
  const v = [0];
  for (let i = 1; i <= n; i++) v.push(v[i - 1] + 0.5 * (a[i] + a[i - 1]) * dt);
  return { t, a, v };
}

function cycleSvg(c) {
  const W = 880;
  const H = 350;
  const L = 58;
  const R = 12;
  const aT = 30;
  const aB = 178;
  const vT = 206;
  const vB = 300;
  const { t, a, v } = cycleCurves();
  const x = (s) => L + ((s - CYCLE.t0) / (CYCLE.t1 - CYCLE.t0)) * (W - L - R);
  const range = (arr, pad) => {
    const lo = Math.min(...arr);
    const hi = Math.max(...arr);
    return [lo - (hi - lo) * pad, hi + (hi - lo) * pad];
  };
  const [aLo, aHi] = range(a, 0.08);
  const [vLo, vHi] = range(v, 0.12);
  const ya = (g) => aB - ((g - aLo) / (aHi - aLo)) * (aB - aT);
  const yv = (m) => vB - ((m - vLo) / (vHi - vLo)) * (vB - vT);
  const path = (xs, ys, fy) => xs.map((s, i) => (i ? "L" : "M") + x(s).toFixed(1) + " " + fy(ys[i]).toFixed(1)).join(" ");

  const names = c.phaseNames;
  const bands = CYCLE_BANDS.map(([from, to, p], k) => {
    const x0 = x(from);
    const x1 = x(to);
    return '<g class="g-band" data-p="' + p + '" tabindex="0" role="button" aria-label="' + names[p] + '">' +
      '<rect x="' + x0.toFixed(1) + '" y="' + aT + '" width="' + (x1 - x0).toFixed(1) + '" height="' + (aB - aT) + '" fill="' + PHASE[p] + '"/>' +
      '<rect x="' + x0.toFixed(1) + '" y="' + vT + '" width="' + (x1 - x0).toFixed(1) + '" height="' + (vB - vT) + '" fill="' + PHASE[p] + '"/>' +
      '<rect class="g-hit" x="' + x0.toFixed(1) + '" y="' + (aT - 22) + '" width="' + (x1 - x0).toFixed(1) + '" height="' + (vB - aT + 22) + '"/>' +
      '<text x="' + ((x0 + x1) / 2).toFixed(1) + '" y="' + (aT - 9) + '" text-anchor="middle">' + names[p] + "</text></g>";
  }).join("");

  let ticks = "";
  for (let s = 0; s <= 2; s += 0.5) {
    ticks += '<line x1="' + x(s) + '" y1="' + vB + '" x2="' + x(s) + '" y2="' + (vB + 4) + '" stroke="' + ZERO + '"/>' +
      '<text x="' + x(s) + '" y="' + (vB + 17) + '" text-anchor="middle">' + guideNumber(s, 1) + "</text>";
  }
  const zero = ya(0);
  return '<svg class="g-cycle" viewBox="0 0 ' + W + " " + H + '" role="img" aria-label="' + c.cycle.aria + '">' +
    bands +
    '<rect x="' + L + '" y="' + aT + '" width="' + (W - L - R) + '" height="' + (aB - aT) + '" fill="none" stroke="' + EDGE + '"/>' +
    '<rect x="' + L + '" y="' + vT + '" width="' + (W - L - R) + '" height="' + (vB - vT) + '" fill="none" stroke="' + EDGE + '"/>' +
    '<line x1="' + L + '" y1="' + zero.toFixed(1) + '" x2="' + (W - R) + '" y2="' + zero.toFixed(1) + '" stroke="' + ZERO + '" stroke-dasharray="4 4"/>' +
    '<text class="g-tick" x="' + (L - 6) + '" y="' + (zero + 4).toFixed(1) + '" text-anchor="end">0</text>' +
    '<text class="g-tick" x="' + (L - 6) + '" y="' + (aT + 12) + '" text-anchor="end">+</text>' +
    '<text class="g-tick" x="' + (L - 6) + '" y="' + (aB - 4) + '" text-anchor="end">−</text>' +
    '<path d="' + path(t, a, ya) + '" fill="none" stroke="' + GUIDE_AXES.surge + '" stroke-width="2.5" stroke-linejoin="round" pointer-events="none"/>' +
    '<path d="' + path(t, v, yv) + '" fill="none" stroke="' + "#1f7a4d" + '" stroke-width="2.5" stroke-linejoin="round" pointer-events="none"/>' +
    '<text class="g-axis" transform="translate(14 ' + ((aT + aB) / 2) + ') rotate(-90)" text-anchor="middle">' + c.cycle.accel + "</text>" +
    '<text class="g-axis" transform="translate(14 ' + ((vT + vB) / 2) + ') rotate(-90)" text-anchor="middle">' + c.cycle.speed + "</text>" +
    ticks +
    '<text class="g-axis" x="' + ((L + W - R) / 2) + '" y="' + (H - 12) + '" text-anchor="middle">' + c.cycle.time + "</text></svg>";
}

function cycleCards(c) {
  return c.cycle.phases.map((p, i) =>
    '<article class="g-phase" data-p="' + i + '" style="--phase:' + PHASE[i] + '">' +
    "<h3>" + c.phaseNames[i] + "</h3>" +
    "<dl><dt>" + c.cycle.happens + "</dt><dd>" + p.happens + "</dd>" +
    "<dt>" + c.cycle.measure + "</dt><dd>" + p.measure + "</dd>" +
    "<dt>" + c.cycle.look + "</dt><dd>" + p.look + "</dd></dl></article>").join("");
}

function guideSetPhase(p) {
  guide.phase = p;
  for (const el of document.querySelectorAll("#guide .g-phase")) el.classList.toggle("on", Number(el.dataset.p) === p);
  for (const el of document.querySelectorAll("#guide .g-band")) el.classList.toggle("on", Number(el.dataset.p) === p);
}

// ---- sensors and outing cards ----

function sourceCards(c) {
  const s = c.sources;
  return s.items.map((it) =>
    '<article class="g-card g-source ' + it.key + '">' +
    "<h3>" + it.name + "</h3>" +
    '<p class="g-measures">' + it.measures + "</p>" +
    '<div class="g-label">' + s.gives + "</div><ul>" + it.gives.map((x) => "<li>" + x + "</li>").join("") + "</ul>" +
    '<div class="g-label">' + s.limit + '</div><p class="g-limit">' + it.limit + "</p></article>").join("");
}

function outingCards(c) {
  const o = c.outing;
  return o.cards.map((it) =>
    '<article class="g-card g-outing">' +
    '<div class="g-tag">' + it.tag + "</div><h3>" + it.title + "</h3>" +
    '<div class="g-label">' + o.see + "</div><p>" + it.see + "</p>" +
    '<div class="g-label good">' + o.deduce + "</div><p>" + it.deduce + "</p>" +
    '<div class="g-label warn">' + o.cannot + "</div><p>" + it.cannot + "</p></article>").join("");
}

function axesSvg(c) {
  const a = c.outing.axes;
  const arrow = (id, color) => '<marker id="' + id + '" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" fill="' + color + '"/></marker>';
  const col = GUIDE_AXES;
  return '<svg class="g-axes" viewBox="0 0 640 230" role="img" aria-label="' + a.aria + '">' +
    "<defs>" + arrow("g-a-surge", col.surge) + arrow("g-a-sway", col.sway) + arrow("g-a-yaw", col.yaw) + arrow("g-a-roll", col.roll) + "</defs>" +
    '<text class="g-view" x="20" y="22">' + a.top + "</text>" +
    // hull from above, bow to the right
    '<path d="M40 118 C90 92 250 92 330 118 C250 144 90 144 40 118 Z" fill="#faf8f3" stroke="' + GUIDE_INK + '" stroke-width="2"/>' +
    '<path d="M330 118 C345 118 352 118 360 118" stroke="' + GUIDE_INK + '" stroke-width="2" fill="none"/>' +
    '<text class="g-small" x="364" y="122">' + a.bow + "</text>" +
    '<line x1="70" y1="64" x2="300" y2="64" stroke="' + col.surge + '" stroke-width="3" marker-end="url(#g-a-surge)"/>' +
    '<text class="g-lab" x="185" y="54" text-anchor="middle" fill="' + col.surge + '">' + a.surge + "</text>" +
    '<line x1="185" y1="84" x2="185" y2="154" stroke="' + col.sway + '" stroke-width="3" marker-start="url(#g-a-sway)" marker-end="url(#g-a-sway)"/>' +
    '<text class="g-lab" x="197" y="172" fill="' + col.sway + '">' + a.sway + "</text>" +
    '<path d="M300 78 A44 44 0 0 1 300 158" fill="none" stroke="' + col.yaw + '" stroke-width="3" marker-start="url(#g-a-yaw)" marker-end="url(#g-a-yaw)"/>' +
    '<text class="g-lab" x="332" y="84" fill="' + col.yaw + '">' + a.yaw + "</text>" +
    // hull from behind, tipped
    '<text class="g-view" x="450" y="22">' + a.rear + "</text>" +
    '<line x1="440" y1="170" x2="620" y2="170" stroke="#9db8d3" stroke-width="2" stroke-dasharray="5 5"/>' +
    '<g transform="rotate(10 530 150)"><path d="M480 128 C480 176 580 176 580 128 Z" fill="#faf8f3" stroke="' + GUIDE_INK + '" stroke-width="2"/>' +
    '<line x1="530" y1="128" x2="530" y2="84" stroke="' + GUIDE_INK + '" stroke-width="2"/><circle cx="530" cy="80" r="7" fill="' + GUIDE_INK + '"/></g>' +
    '<path d="M474 64 A62 62 0 0 1 590 64" fill="none" stroke="' + col.roll + '" stroke-width="3" marker-start="url(#g-a-roll)" marker-end="url(#g-a-roll)"/>' +
    '<text class="g-lab" x="532" y="44" text-anchor="middle" fill="' + col.roll + '">' + a.roll + "</text></svg>";
}

function axesNotes(c) {
  return c.outing.axes.notes.map(([key, text]) =>
    '<li><i style="background:' + GUIDE_AXES[key] + '"></i><b>' + c.outing.axes[key] + "</b> " + text + "</li>").join("");
}

// ---- the real stroke ----

function sampleOf(ts, vs, t) {
  if (!ts.length || t < ts[0] || t > ts[ts.length - 1]) return null;
  let lo = 0;
  let hi = ts.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (ts[mid] <= t) lo = mid; else hi = mid;
  }
  const f = ts[hi] === ts[lo] ? 0 : (t - ts[lo]) / (ts[hi] - ts[lo]);
  return vs[lo] + (vs[hi] - vs[lo]) * f;
}

// Where each numbered mark sits: a point on the curve (panel "a" or "v") and where its bubble goes.
function exampleMarks(d) {
  const m = d.markers;
  const c = d.curve;
  const speedAt = d.now.speed_peak;
  const marks = [];
  marks.push({ panel: "a", t: m.depth_t, y: m.depth_g, dx: -30, dy: 6 });
  marks.push({ panel: "a", t: m.peak_t, y: m.peak_g, dx: 0, dy: -24 });
  if (m.sag_t != null) marks.push({ panel: "a", t: m.sag_t, y: m.sag_g, dx: 0, dy: 26 });
  if (m.t80 != null && m.t20 != null) {
    marks.push({ panel: "a", t: (m.t80 + m.t20) / 2, y: (m.level80 + m.level20) / 2, dx: 26, dy: -18 });
  }
  if (m.release_t != null) marks.push({ panel: "a", t: m.release_t, y: m.release_g, dx: 0, dy: 26 });
  const v = speedAt != null ? sampleOf(c.t, c.v, speedAt) : null;
  if (v != null) marks.push({ panel: "v", t: speedAt, y: v, dx: 0, dy: -24 });
  return marks;
}

function exampleDraw() {
  const d = guide.example;
  const canvas = $("g-canvas");
  if (!canvas || !d || !d.has) return;
  const box = canvas.parentElement;
  const w = box.clientWidth;
  if (w < 120) return;
  const h = Math.max(340, Math.min(540, Math.round(w * 0.8)));
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  canvas.style.height = h + "px";
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const g = guideText();
  const c = d.curve;
  const b = d.bounds;
  const t0 = c.t[0];
  const t1 = c.t[c.t.length - 1];
  const L = 50;
  const R = 10;
  const aT = 8;
  const aB = Math.round(h * 0.6);
  const vT = aB + 16;
  const vB = h - 26;
  const X = (t) => L + ((t - t0) / (t1 - t0)) * (w - L - R);
  const span = (arr, padLo, padHi) => {
    const lo = Math.min(...arr);
    const hi = Math.max(...arr);
    return [lo - (hi - lo) * padLo, hi + (hi - lo) * padHi];
  };
  const [aLo, aHi] = span(c.a, 0.14, 0.26);
  const [vLo, vHi] = span(c.v, 0.12, 0.4);
  const Ya = (x) => aB - ((x - aLo) / (aHi - aLo)) * (aB - aT);
  const Yv = (x) => vB - ((x - vLo) / (vHi - vLo)) * (vB - vT);

  // phases, behind everything
  const spans = [
    [Math.min(b.catch0, 0), 0, 4, false],
    [0, b.catch1, 0, true],
    [b.catch1, b.finish, 1, true],
    [b.finish, b.finish_end, 2, true],
    [b.finish_end, b.approach, 3, true],
    [b.approach, b.interval, 4, true],
  ];
  ctx.font = "600 11px " + FONT;
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (const [from, to, p, named] of spans) {
    const u0 = Math.max(t0, from);
    const u1 = Math.min(t1, to);
    if (u1 - u0 < 0.01) continue;
    ctx.globalAlpha = 0.6;
    ctx.fillStyle = PHASE[p];
    ctx.fillRect(X(u0), aT, X(u1) - X(u0), aB - aT);
    ctx.fillRect(X(u0), vT, X(u1) - X(u0), vB - vT);
    ctx.globalAlpha = 1;
    const name = g.phaseNames[p];
    if (named && X(u1) - X(u0) > ctx.measureText(name).width + 8) {
      ctx.fillStyle = "#4a443a";
      ctx.fillText(name, (X(u0) + X(u1)) / 2, aT + 5);
    }
  }
  ctx.strokeStyle = EDGE;
  ctx.lineWidth = 1;
  ctx.strokeRect(L + 0.5, aT + 0.5, w - L - R, aB - aT);
  ctx.strokeRect(L + 0.5, vT + 0.5, w - L - R, vB - vT);

  // grid and labels
  ctx.font = "11px " + FONT;
  ctx.fillStyle = MUTED;
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let a = Math.ceil(aLo * 5) / 5; a <= aHi; a += 0.2) {
    const y = Ya(a);
    ctx.strokeStyle = Math.abs(a) < 1e-6 ? ZERO : GRID;
    ctx.setLineDash(Math.abs(a) < 1e-6 ? [4, 4] : []);
    ctx.beginPath();
    ctx.moveTo(L, y);
    ctx.lineTo(w - R, y);
    ctx.stroke();
    ctx.fillText(guideNumber(a, 1), L - 6, y);
  }
  ctx.setLineDash([]);
  for (const v of [Math.ceil(vLo * 10) / 10, Math.floor(vHi * 10) / 10]) {
    ctx.fillText(guideNumber(v, 1), L - 6, Yv(v));
  }
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let t = Math.ceil(t0 * 2) / 2; t <= t1; t += 0.5) {
    ctx.strokeStyle = GRID;
    ctx.beginPath();
    ctx.moveTo(X(t), aT);
    ctx.lineTo(X(t), vB);
    ctx.stroke();
    ctx.fillText(guideNumber(t, 1) + " s", X(t), vB + 6);
  }
  ctx.save();
  ctx.fillStyle = MUTED;
  ctx.font = "600 10.5px " + FONT;
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.fillText(g.example.accel.toUpperCase(), L + 6, aT + 22);
  ctx.fillText(g.example.speed.toUpperCase(), L + 6, vT + 5);
  ctx.restore();

  // the curves
  const line = (ys, Y, color) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2.25;
    ctx.lineJoin = "round";
    ctx.beginPath();
    c.t.forEach((t, i) => (i ? ctx.lineTo(X(t), Y(ys[i])) : ctx.moveTo(X(t), Y(ys[i]))));
    ctx.stroke();
  };
  line(c.a, Ya, GUIDE_AXES.surge);
  line(c.v, Yv, "#1f7a4d");

  // the marks, then the numbered bubbles
  const m = d.markers;
  ctx.lineWidth = 1.5;
  ctx.strokeStyle = "rgba(28, 25, 21, 0.55)";
  if (m.width_l != null && m.width_r != null && m.half_level != null) {
    ctx.beginPath();
    ctx.moveTo(X(m.width_l), Ya(m.half_level));
    ctx.lineTo(X(m.width_r), Ya(m.half_level));
    ctx.moveTo(X(m.width_l), Ya(m.half_level) - 5);
    ctx.lineTo(X(m.width_l), Ya(m.half_level) + 5);
    ctx.moveTo(X(m.width_r), Ya(m.half_level) - 5);
    ctx.lineTo(X(m.width_r), Ya(m.half_level) + 5);
    ctx.stroke();
  }
  if (m.t80 != null && m.t20 != null) {
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(X(m.t80), Ya(m.level80));
    ctx.lineTo(X(m.t20), Ya(m.level80));
    ctx.moveTo(X(m.t80), Ya(m.level20));
    ctx.lineTo(X(m.t20), Ya(m.level20));
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.beginPath();
    ctx.moveTo(X(m.t20), Ya(m.level80));
    ctx.lineTo(X(m.t20), Ya(m.level20));
    ctx.stroke();
  }
  exampleMarks(d).forEach((mk, i) => {
    const hot = guide.hot === i + 1;
    const px = X(mk.t);
    const py = (mk.panel === "a" ? Ya : Yv)(mk.y);
    const bx = Math.max(L + 12, Math.min(w - R - 12, px + mk.dx));
    const by = py + mk.dy;
    ctx.strokeStyle = hot ? GUIDE_HOT : "rgba(28, 25, 21, 0.55)";
    ctx.lineWidth = hot ? 2 : 1.25;
    ctx.beginPath();
    ctx.moveTo(px, py);
    ctx.lineTo(bx, by);
    ctx.stroke();
    ctx.fillStyle = hot ? GUIDE_HOT : GUIDE_INK;
    ctx.beginPath();
    ctx.arc(px, py, hot ? 5 : 3.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.lineWidth = 2;
    ctx.strokeStyle = "#fff";
    ctx.beginPath();
    ctx.arc(bx, by, hot ? 13 : 11, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
    ctx.fillStyle = "#fff";
    ctx.font = "700 " + (hot ? 13 : 12) + "px " + FONT;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(String(i + 1), bx, by + 0.5);
  });
}

function exampleFill() {
  const note = $("g-note");
  const card = document.querySelector("#guide .g-example");
  const lead = $("g-example-lead");
  if (!note || !card) return;
  const g = guideText();
  const d = guide.example;
  if (!d || !d.has) {
    note.hidden = false;
    note.textContent = d ? g.example.missing : g.example.loading;
    $("g-callouts").innerHTML = "";
    return;
  }
  note.hidden = true;
  const nf = guideNumber;
  lead.textContent = g.example.lead(d, nf);
  const marks = exampleMarks(d);
  // the bubbles follow the marks that exist; the texts are in the same order, minus the ones left out
  const texts = [];
  const mk = d.markers;
  const present = [true, true, mk.sag_t != null, mk.t80 != null && mk.t20 != null, mk.release_t != null, marks.some((x) => x.panel === "v")];
  g.example.marks.forEach((make, i) => { if (present[i]) texts.push(make(d, nf)); });
  $("g-callouts").innerHTML = texts.map((tx, i) =>
    '<li data-n="' + (i + 1) + '"><span class="g-num">' + (i + 1) + "</span><div><b>" + tx.title + "</b><p>" + tx.text + "</p></div></li>").join("");
  for (const li of document.querySelectorAll("#g-callouts li")) {
    const n = Number(li.dataset.n);
    li.addEventListener("mouseenter", () => { guide.hot = n; exampleDraw(); });
    li.addEventListener("mouseleave", () => { guide.hot = 0; exampleDraw(); });
  }
  exampleDraw();
}

async function guideLoadExample() {
  if (guide.loading || (guide.example && guide.example.has)) return;
  guide.loading = true;
  try {
    guide.example = await getJSON("/api/guide");
  } catch (err) {
    guide.example = { has: false };
  }
  guide.loading = false;
  exampleFill();
}

// ---- the page ----

function guideRender() {
  const c = guideText();
  c.phaseNames = COPY[lang()].phases;
  guide.lang = lang();
  guide.hot = 0;
  $("guide").innerHTML =
    '<div class="g-wrap">' +
    '<header class="g-hero"><h1>' + c.title + "</h1><p>" + c.lead + "</p></header>" +
    '<section class="g-sec"><h2>' + c.sources.title + "</h2><p class=\"g-lead\">" + c.sources.lead + "</p>" +
    '<div class="g-grid sources">' + sourceCards(c) + "</div></section>" +
    '<section class="g-sec"><h2>' + c.cycle.title + "</h2><p class=\"g-lead\">" + c.cycle.lead + "</p>" +
    '<div class="g-card g-scroll">' + cycleSvg(c) + "</div>" +
    '<div class="g-phases">' + cycleCards(c) + "</div></section>" +
    '<section class="g-sec"><h2>' + c.example.title + '</h2><p class="g-lead" id="g-example-lead"></p>' +
    '<div class="g-card g-example"><div class="g-canvas"><canvas id="g-canvas" role="img" aria-label="' + c.example.aria + '"></canvas>' +
    '<div class="g-note" id="g-note">' + c.example.loading + "</div></div>" +
    '<ol class="g-callouts" id="g-callouts"></ol></div></section>' +
    '<section class="g-sec"><h2>' + c.outing.title + "</h2><p class=\"g-lead\">" + c.outing.lead + "</p>" +
    '<div class="g-card g-boat"><div class="g-scroll">' + axesSvg(c) + "</div><div><h3>" + c.outing.axes.title + "</h3><ul>" + axesNotes(c) + "</ul></div></div>" +
    '<div class="g-grid outing">' + outingCards(c) + "</div></section>" +
    "</div>";

  for (const el of document.querySelectorAll("#guide .g-band")) {
    const p = Number(el.dataset.p);
    el.addEventListener("mouseenter", () => guideSetPhase(p));
    el.addEventListener("focus", () => guideSetPhase(p));
    el.addEventListener("click", () => guideSetPhase(p));
  }
  for (const el of document.querySelectorAll("#guide .g-phase")) {
    const p = Number(el.dataset.p);
    el.addEventListener("mouseenter", () => guideSetPhase(p));
  }
  guideSetPhase(1);
  exampleFill();
}

function guideShow() {
  if (guide.lang !== lang()) guideRender();
  guideLoadExample();
  requestAnimationFrame(exampleDraw);
}

$("lang").addEventListener("change", () => {
  if (document.body.classList.contains("page-guide")) guideRender();
  else guide.lang = null;
});
if (typeof ResizeObserver !== "undefined") {
  let frame = 0;
  new ResizeObserver(() => {
    if (frame || !document.body.classList.contains("page-guide")) return;
    frame = requestAnimationFrame(() => { frame = 0; exampleDraw(); });
  }).observe($("guide"));
}
