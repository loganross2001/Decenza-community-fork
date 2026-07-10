#pragma once

// [barista-fork] Spoken-text normalization for the assistant voices.
//
// The barista + coaching voices speak strings that contain compact espresso
// notation — "18.8g", "1:2.4", "93°C" — that every TTS engine (native macOS/
// Android, OpenAI, ElevenLabs) mispronounces: "g" comes out as the letter
// "gee"/"Gs" instead of "grams", and the ratio "1:2.4" is read as a garbled
// clock time / fraction rather than "one to two point four".
//
// normalizeForSpeech() rewrites ONLY these clear unit/ratio patterns into
// spoken words. It is deliberately conservative: every rule requires a numeric
// context and a trailing non-letter boundary, so it never pluralizes random
// words or mangles multi-letter units it doesn't recognize (kg, mg, …).
//
// This transforms SPOKEN text only. It is applied in AssistantVoice::speak()
// (the single dispatch point for all providers + both roles) — the on-screen
// chat text is a separate string and is left untouched.
//
// Header-only + free function on purpose: it depends on nothing but QString +
// QRegularExpression (both in Qt6::Core), so it is trivially unit-testable
// without constructing an AssistantVoice (which drags in TextToSpeech /
// Multimedia / Network). See tests/tst_speechnormalize.cpp.

#include <QString>
#include <QRegularExpression>

namespace speechnormalize {

// Spell a single decimal digit 0-9 as a word (used for the ratio decimal part,
// which engines otherwise read unreliably). Anything else is returned as-is.
inline QString digitWord(QChar d) {
    switch (d.unicode()) {
        case u'0': return QStringLiteral("zero");
        case u'1': return QStringLiteral("one");
        case u'2': return QStringLiteral("two");
        case u'3': return QStringLiteral("three");
        case u'4': return QStringLiteral("four");
        case u'5': return QStringLiteral("five");
        case u'6': return QStringLiteral("six");
        case u'7': return QStringLiteral("seven");
        case u'8': return QStringLiteral("eight");
        case u'9': return QStringLiteral("nine");
        default:   return QString(d);
    }
}

// Spell a small integer 0-9 as a word; larger integers are left as digits (the
// engines read plain integers like "12" fine — it's the colon + decimal that
// break them, which the ratio rewrite removes).
inline QString smallIntWord(const QString& n) {
    if (n.size() == 1)
        return digitWord(n.at(0));
    return n;
}

inline QString normalizeForSpeech(const QString& text) {
    QString out = text;

    // ---- Pass 1: espresso ratios  x:y  and  x:y.z  ---------------------------
    // Match ONLY the brew-ratio shape: a single-digit integer, a colon, then a
    // single-digit integer with an optional decimal part. The single-digit
    // left/right sides (plus the lookarounds below) keep this away from clock
    // times like "1:30" (two-digit right, no decimal) and any longer number.
    //   "1:2"     -> "one to two"
    //   "1:2.4"   -> "one to two point four"
    //   "1:2.35"  -> "one to two point three five"
    // The decimal digits are spelled individually ("point three five") because
    // engines read "2.35" unreliably in this context.
    // Run BEFORE the unit pass so a ratio's digits never get "grams"-ified.
    // Trailing guard is (?!\w) — NOT (?![\w.]) — so a sentence-final ratio still
    // converts: "Aim for a 1:2." must speak "one to two", but (?![\w.]) would
    // see the period and refuse to match. (?!\w) still blocks clock times: the
    // greedy optional decimal fails on "1:30" (no dot), leaving a digit after
    // the colon that (?!\w) rejects. Leading (?<![\w.]) still bars mid-number.
    static const QRegularExpression ratioRe(
        QStringLiteral("(?<![\\w.])(\\d):(\\d)(?:\\.(\\d+))?(?!\\w)"));
    {
        QString rebuilt;
        qsizetype last = 0;
        auto it = ratioRe.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            rebuilt += out.mid(last, m.capturedStart() - last);
            rebuilt += smallIntWord(m.captured(1));   // left integer
            rebuilt += QStringLiteral(" to ");
            rebuilt += smallIntWord(m.captured(2));    // right integer
            const QString frac = m.captured(3);
            if (!frac.isEmpty()) {
                rebuilt += QStringLiteral(" point");
                for (QChar c : frac)                    // "35" -> " three five"
                    rebuilt += QLatin1Char(' ') + digitWord(c);
            }
            last = m.capturedEnd();
        }
        rebuilt += out.mid(last);
        out = rebuilt;
    }

    // ---- Pass 2: units after a number  --------------------------------------
    // Each rule is <number><unit> with a trailing NON-LETTER lookahead so it
    // never fires inside a longer token: "18.8g"->grams but "5kg" untouched;
    // "27s"->seconds but "1st"/"gestures" untouched. The number itself is left
    // as digits (engines read "18.8" fine — the bare unit letter is the bug).
    // Order matters: multi-letter units and °C/°F run before bare ° and bare
    // single-letter units so the longer match wins.
    struct Rule { const char* pattern; const char* repl; };
    static const Rule rules[] = {
        // Temperature: both °C and °F collapse to "degrees" (per owner: the
        // scale word isn't spoken). "93°C"/"93°F"/"93C" (clear temp) -> "93 degrees".
        { "(\\d)\\s*°\\s*[CF]\\b",   "\\1 degrees" },
        { "(\\d)\\s*°",              "\\1 degrees" },   // "93°" with no letter
        { "(\\d)C(?![A-Za-z])",      "\\1 degrees" },   // bare "93C" temp shorthand
        // Weight: "18g" / "18.8g" -> grams. Lookahead blocks kg/mg/…
        { "(\\d)g(?![A-Za-z])",      "\\1 grams" },
        // Volume: "30ml" -> milliliters.
        { "(\\d)ml(?![A-Za-z])",     "\\1 milliliters" },
        // Duration: "27s" -> seconds. Numeric prefix + non-letter guard keeps
        // it off ordinals ("1st") and words.
        { "(\\d)s(?![A-Za-z])",      "\\1 seconds" },
        // Pressure / speed: spell the acronym letter-wise so it's not read as
        // a word. "9psi"->"9 P S I", "9000rpm"->"9000 R P M".
        { "(\\d)\\s*psi\\b",         "\\1 P S I" },
        { "(\\d)\\s*rpm\\b",         "\\1 R P M" },
        { "(\\d)\\s*RPM\\b",         "\\1 R P M" },
    };
    for (const Rule& r : rules) {
        // fromUtf8, not fromLatin1: the °C/°F/° patterns contain the degree sign
        // (U+00B0), which this source file stores as UTF-8 — Latin-1 decoding
        // would corrupt it and the temperature rules would never match.
        QRegularExpression re(QString::fromUtf8(r.pattern));
        out.replace(re, QString::fromUtf8(r.repl));
    }

    return out;
}

}  // namespace speechnormalize
