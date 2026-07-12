#pragma once

#include <QString>
#include <QStringList>

#include <cmath>

// Drink-type helpers for the recipe wizard (add-recipe-wizard-tea).
//
// Tea profile recommendation has two data-driven signals. `teaProfileMatchesType`
// (type match) is the shared authority used by both the wizard's ranking and
// the unit tests. `teaTempProximity` (temp proximity) is the reference/tested
// implementation of the tier-③ ordering key; the wizard reimplements the
// ordering inline in QML (RecipeWizardPage.qml) with the same distance metric.
//
//  1. Type match: the stock Decent tea profiles encode the tea type in their
//     TITLE ("Tea portafilter/black tea", ".../Sencha", ".../Oolong 1st
//     extraction", ".../tisane"). A bag whose extracted teaType matches ranks
//     that profile at the top of the similar tier with a reason chip. This is
//     a ranking signal, not a gate — a miss degrades to temp proximity, and
//     profile search is always available.
//
//  2. Temperature proximity: for profiles with no recognizable type in the
//     title (user-imported), order by |profile temp − the bag's stated
//     brewTempC|. The profile temps themselves encode the style (Sencha 60°,
//     Japanese green 74°, white 80°, oolong 88–94°, black 105°).
//
// Tea default temperatures (design D8): used by the wizard when the vendor
// stated nothing — sensible per-type steeping classes, never guessed per-bag.

namespace DrinkTypes {

// Does this profile title name the given tea type? Keyword table over the 15
// stock tea_portafilter titles; matching is case-insensitive substring on the
// title. "sensha" covers the stock Blue Willow Tsuyuhikari Sensha spelling.
inline bool teaProfileMatchesType(const QString& profileTitle, const QString& teaType)
{
    const QString title = profileTitle.toLower();
    const QString type = teaType.trimmed().toLower();
    if (title.isEmpty() || type.isEmpty())
        return false;

    struct Keyword { const char* type; const char* titleWord; };
    static const Keyword kKeywords[] = {
        { "black",  "black tea" },
        { "green",  "green" },      // Japanese/Chinese/Yunnan green
        { "green",  "sencha" },
        { "green",  "sensha" },
        { "oolong", "oolong" },
        { "white",  "white tea" },
        { "herbal", "tisane" },
        { "pu-erh", "pu-erh" },
        { "pu-erh", "puerh" },
        { "pu-erh", "pu erh" },
    };
    for (const Keyword& k : kKeywords) {
        if (type == QLatin1String(k.type) && title.contains(QLatin1String(k.titleWord)))
            return true;
    }
    return false;
}

// Tier-③ tea ordering: distance between the profile's temperature and the
// bag's stated brew temperature. Callers sort ascending and fall back to
// alphabetical when the bag states no temp (brewTempC <= 0 → every profile
// keys 0, preserving the caller's alphabetical pre-sort).
inline double teaTempProximity(double profileTempC, double brewTempC)
{
    if (brewTempC <= 0 || profileTempC <= 0)
        return 0;
    return std::abs(profileTempC - brewTempC);
}

// Does this recipe drink type carry a grind? Tea family ("tea",
// "tea_hotwater") stores no grind — a dial edit while a tea recipe is active
// must not turn it into a recipe that grinds (fix-recipe-grind-integrity).
// An empty type reads as coffee: recipes without a stored drink_type predate
// migration 28, which shipped together with tea recipes, so a stored-empty
// tea recipe does not occur in practice.
inline bool hasGrind(const QString& drinkType)
{
    return !drinkType.startsWith(QLatin1String("tea"));
}

// Per-type default brew temperature (Celsius) for tea bags whose vendor
// stated nothing. Steeping classes, not per-bag guesses.
inline double defaultTeaTempC(const QString& teaType)
{
    const QString type = teaType.trimmed().toLower();
    if (type == QLatin1String("green") || type == QLatin1String("white"))
        return 80;
    if (type == QLatin1String("oolong"))
        return 90;
    if (type == QLatin1String("pu-erh"))
        return 95;
    if (type == QLatin1String("herbal"))
        return 100;
    if (type == QLatin1String("black"))
        return 98;
    return 90;  // unknown type: middle of the road
}

// --- Espresso shot type (for natural, non-stat-dump shot talk) --------------
// Classify a shot by its yield:dose ratio into the espresso type the barista
// speaks in. Ranges follow the coffee convention the barista persona already
// uses: ristretto is short (~1:1–1:1.5), normale (~1:2–1:2.5), lungo (~1:3 and
// longer). Boundaries sit in the GAPS between ranges so a shot lands in the
// nearest bucket. Returns "" for a non-espresso / unknown ratio (<= 0).
// NOTE: these are the fixed CONVENTION ranges, not the user's editable ratio
// presets — it's a speakable hint, and the persona also gives the model the
// user's own preset dial-points and tells it to qualify near-boundary shots.
inline QString espressoTypeFromRatio(double ratio)
{
    if (ratio <= 0.0) return QString();
    if (ratio < 1.75) return QStringLiteral("ristretto");
    if (ratio < 2.75) return QStringLiteral("normale");
    return QStringLiteral("lungo");
}

// A short, speakable descriptor for a shot — the natural way to refer to it
// ("a normale espresso on the Kenya beans") instead of reciting its numbers.
// Composed as "<type> espresso on the <bean> beans"; the bean phrase prefers
// the roaster/origin label (bean_brand, usually the origin) and falls back to
// the varietal (bean_type). Degrades gracefully: no bean → just "<type>
// espresso"; unknown ratio → "espresso". The words "espresso" and "beans" are
// deliberate — they make the phrase read like speech, not a spec.
inline QString espressoShotDescriptor(double ratio, const QString& roaster, const QString& bean)
{
    const QString type = espressoTypeFromRatio(ratio);
    QString head = type.isEmpty() ? QStringLiteral("espresso")
                                  : type + QStringLiteral(" espresso");
    const QString beanName = !roaster.trimmed().isEmpty() ? roaster.trimmed() : bean.trimmed();
    if (beanName.isEmpty())
        return head;
    return head + QStringLiteral(" on the ") + beanName + QStringLiteral(" beans");
}

} // namespace DrinkTypes
