/* Keep SceShell's selection policy separate from the kernel's codecs/table. */
#include "text_source.h"

uint32_t vjo_text_auto_select(const VjoTextCandidate *c, unsigned n)
{
    if (!n || !c[0].id || !c[0].text[0] || !c[0].japanese || c[0].length < 4 ||
        c[0].score < VJO_TEXT_MATCH_PERCENT) return 0;
    if (n > 1 && c[1].japanese && c[1].score == c[0].score) return 0;
    return c[0].id;
}
