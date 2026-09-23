#include "i18n.h"

namespace ui {

namespace {
// ponytail: module state rather than a parameter on every call. There is one overlay per process
// and one language at a time; threading it through every T at every call site would be three
// hundred copies of the same argument.
int g_language = 0;
}

void SetLanguage(int language)
{
    g_language = language;
}

const char *T(const char *en, const char *pt)
{
    return g_language == 0 ? en : pt;
}

} // namespace ui
