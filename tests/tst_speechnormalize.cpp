#include <QTest>

#include "barista/speechnormalize.h"

using speechnormalize::normalizeForSpeech;

// [barista-fork] Guards normalizeForSpeech() — the spoken-text rewrite that turns
// compact espresso notation ("18.8g", "1:2.4", "93°C") into words the TTS engines
// pronounce correctly. Applied once in AssistantVoice::speak() for all providers +
// both roles. The tricky parts are (a) the ratio rewrite running BEFORE the unit
// rewrite so a ratio's digits don't get "grams"-ified, and (b) the non-letter
// guards that keep the unit rules off longer tokens (kg, 1st, words), so those
// get explicit coverage.
class TstSpeechNormalize : public QObject
{
    Q_OBJECT

private slots:
    void grams();
    void gramsDecimal();
    void gramsNotKg();
    void ratioNoDecimal();
    void ratioOneDecimal();
    void ratioTwoDecimals();
    void ratioNotClockTime();
    void ratioSentenceFinal();
    void ratioDecimalSentenceFinal();
    void tempCelsius();
    void tempFahrenheit();
    void tempBareDegree();
    void seconds();
    void secondsNotOrdinal();
    void thousandsSeparatorStripped();
    void thousandsMultiGroup();
    void listCommaUntouched();
    void dayOrdinalSpelled();
    void ordinalOutOfRangeUntouched();
    void shotNamesRespelled();
    void milliliters();
    void psiAndRpm();
    void ratioBeforeUnits();
    void sentenceMixed();
    void leavesPlainTextAlone();
};

void TstSpeechNormalize::grams()
{ QCOMPARE(normalizeForSpeech("18g"), QString("18 grams")); }

void TstSpeechNormalize::gramsDecimal()
{ QCOMPARE(normalizeForSpeech("18.8g"), QString("18.8 grams")); }

void TstSpeechNormalize::gramsNotKg()
{ QCOMPARE(normalizeForSpeech("5kg"), QString("5kg")); }   // multi-letter unit untouched

void TstSpeechNormalize::ratioNoDecimal()
{ QCOMPARE(normalizeForSpeech("1:2"), QString("one to two")); }

void TstSpeechNormalize::ratioOneDecimal()
{ QCOMPARE(normalizeForSpeech("1:2.4"), QString("one to two point four")); }

void TstSpeechNormalize::ratioTwoDecimals()
{ QCOMPARE(normalizeForSpeech("1:2.35"), QString("one to two point three five")); }

void TstSpeechNormalize::ratioNotClockTime()
// "1:30" is a clock time (two-digit right, no decimal) — the tight brew-ratio
// pattern must NOT rewrite it.
{ QCOMPARE(normalizeForSpeech("1:30"), QString("1:30")); }

void TstSpeechNormalize::ratioSentenceFinal()
// A ratio ending a sentence (trailing period) must still convert — the trailing
// guard is (?!\w), not (?![\w.]), so the period doesn't block the match.
{ QCOMPARE(normalizeForSpeech("Aim for a 1:2."), QString("Aim for a one to two.")); }

void TstSpeechNormalize::ratioDecimalSentenceFinal()
{ QCOMPARE(normalizeForSpeech("Your ratio was 1:2.4."),
           QString("Your ratio was one to two point four.")); }

void TstSpeechNormalize::tempCelsius()
{ QCOMPARE(normalizeForSpeech("93°C"), QString("93 degrees")); }

void TstSpeechNormalize::tempFahrenheit()
{ QCOMPARE(normalizeForSpeech("200°F"), QString("200 degrees")); }

void TstSpeechNormalize::tempBareDegree()
{ QCOMPARE(normalizeForSpeech("93°"), QString("93 degrees")); }

void TstSpeechNormalize::seconds()
{ QCOMPARE(normalizeForSpeech("27s"), QString("27 seconds")); }

void TstSpeechNormalize::secondsNotOrdinal()
{ QCOMPARE(normalizeForSpeech("1st"), QString("first")); }   // seconds rule doesn't fire; ordinal spelled instead

void TstSpeechNormalize::thousandsSeparatorStripped()
{ QCOMPARE(normalizeForSpeech("1,755 shots"), QString("1755 shots")); }   // comma made ElevenLabs stutter

void TstSpeechNormalize::thousandsMultiGroup()
{ QCOMPARE(normalizeForSpeech("1,234,567"), QString("1234567")); }

void TstSpeechNormalize::listCommaUntouched()
{ QCOMPARE(normalizeForSpeech("beans, water and milk"), QString("beans, water and milk")); }

void TstSpeechNormalize::dayOrdinalSpelled()
{
    QCOMPARE(normalizeForSpeech("June 24th"), QString("June twenty-fourth"));
    QCOMPARE(normalizeForSpeech("the 3rd pull"), QString("the third pull"));
    QCOMPARE(normalizeForSpeech("on the 21st"), QString("on the twenty-first"));
}

void TstSpeechNormalize::ordinalOutOfRangeUntouched()
{ QCOMPARE(normalizeForSpeech("1755th place"), QString("1755th place")); }   // >31 → left as digits

void TstSpeechNormalize::shotNamesRespelled()
{
    QCOMPARE(normalizeForSpeech("Try a Normale"), QString("Try a nor-mah-lay"));   // was said "normal-ee"
    QCOMPARE(normalizeForSpeech("a lungo shot"),  QString("a loong-goh shot"));
    QCOMPARE(normalizeForSpeech("RISTRETTO"),     QString("ree-stret-toh"));       // case-insensitive
    QCOMPARE(normalizeForSpeech("normalize it"),  QString("normalize it"));        // word-boundary: no false hit
}

void TstSpeechNormalize::milliliters()
{ QCOMPARE(normalizeForSpeech("36ml"), QString("36 milliliters")); }

void TstSpeechNormalize::psiAndRpm()
{
    QCOMPARE(normalizeForSpeech("9psi"), QString("9 P S I"));
    QCOMPARE(normalizeForSpeech("1200rpm"), QString("1200 R P M"));
}

void TstSpeechNormalize::ratioBeforeUnits()
// The digit after the colon must not be swallowed by the grams/seconds rules —
// ratio rewrite runs first and leaves no digits behind in that span.
{ QCOMPARE(normalizeForSpeech("1:2"), QString("one to two")); }

void TstSpeechNormalize::sentenceMixed()
{
    QCOMPARE(normalizeForSpeech(QStringLiteral("Pull 18.8g at 93°C for a 1:2.4 in 27s.")),
             QString("Pull 18.8 grams at 93 degrees for a one to two point four in 27 seconds."));
}

void TstSpeechNormalize::leavesPlainTextAlone()
{ QCOMPARE(normalizeForSpeech("Ready when you are."), QString("Ready when you are.")); }

QTEST_APPLESS_MAIN(TstSpeechNormalize)
#include "tst_speechnormalize.moc"
