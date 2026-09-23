#pragma once
// Every user-visible string in the overlay goes through T. Two literals at the call site instead of
// an id, a table and a lookup: the translation is then impossible to get out of sync with the text
// it translates, and adding a control cannot leave a dangling key behind.
// ponytail: a real string table earns its keep at a third language, not at two.

namespace ui {

// 0 English, 1 Brazilian Portuguese -- the Language setting's own values. DrawPanel sets it from
// the settings at the top of every frame, so a change made in the panel shows on the next frame.
void SetLanguage(int language);

const char *T(const char *en, const char *pt);

} // namespace ui
