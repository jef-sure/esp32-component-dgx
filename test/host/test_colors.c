/*
 * The names of colors of dgx_colors.h against the colors of CSS (CSS Color Module Level 4, its 148 names written out
 * here): every name of CSS is DGX_<NAME> with the value of CSS; COPPER is DGX's own. Every packer takes them, the 16 bits
 * of dgx_rgb_to_16 as the 24 of DGX_RGB_24.
 */
#include <stdint.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_colors.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* a name of CSS, its value there and that of DGX_<NAME> */
static const struct {
    const char *name;
    uint32_t    css, dgx;
} names[] = {
    {"aliceblue", RGB(240, 248, 255), DGX_ALICEBLUE(RGB)},
    {"antiquewhite", RGB(250, 235, 215), DGX_ANTIQUEWHITE(RGB)},
    {"aqua", RGB(0, 255, 255), DGX_AQUA(RGB)},
    {"aquamarine", RGB(127, 255, 212), DGX_AQUAMARINE(RGB)},
    {"azure", RGB(240, 255, 255), DGX_AZURE(RGB)},
    {"beige", RGB(245, 245, 220), DGX_BEIGE(RGB)},
    {"bisque", RGB(255, 228, 196), DGX_BISQUE(RGB)},
    {"black", RGB(0, 0, 0), DGX_BLACK(RGB)},
    {"blanchedalmond", RGB(255, 235, 205), DGX_BLANCHEDALMOND(RGB)},
    {"blue", RGB(0, 0, 255), DGX_BLUE(RGB)},
    {"blueviolet", RGB(138, 43, 226), DGX_BLUEVIOLET(RGB)},
    {"brown", RGB(165, 42, 42), DGX_BROWN(RGB)},
    {"burlywood", RGB(222, 184, 135), DGX_BURLYWOOD(RGB)},
    {"cadetblue", RGB(95, 158, 160), DGX_CADETBLUE(RGB)},
    {"chartreuse", RGB(127, 255, 0), DGX_CHARTREUSE(RGB)},
    {"chocolate", RGB(210, 105, 30), DGX_CHOCOLATE(RGB)},
    {"coral", RGB(255, 127, 80), DGX_CORAL(RGB)},
    {"cornflowerblue", RGB(100, 149, 237), DGX_CORNFLOWERBLUE(RGB)},
    {"cornsilk", RGB(255, 248, 220), DGX_CORNSILK(RGB)},
    {"crimson", RGB(220, 20, 60), DGX_CRIMSON(RGB)},
    {"cyan", RGB(0, 255, 255), DGX_CYAN(RGB)},
    {"darkblue", RGB(0, 0, 139), DGX_DARKBLUE(RGB)},
    {"darkcyan", RGB(0, 139, 139), DGX_DARKCYAN(RGB)},
    {"darkgoldenrod", RGB(184, 134, 11), DGX_DARKGOLDENROD(RGB)},
    {"darkgray", RGB(169, 169, 169), DGX_DARKGRAY(RGB)},
    {"darkgreen", RGB(0, 100, 0), DGX_DARKGREEN(RGB)},
    {"darkgrey", RGB(169, 169, 169), DGX_DARKGREY(RGB)},
    {"darkkhaki", RGB(189, 183, 107), DGX_DARKKHAKI(RGB)},
    {"darkmagenta", RGB(139, 0, 139), DGX_DARKMAGENTA(RGB)},
    {"darkolivegreen", RGB(85, 107, 47), DGX_DARKOLIVEGREEN(RGB)},
    {"darkorange", RGB(255, 140, 0), DGX_DARKORANGE(RGB)},
    {"darkorchid", RGB(153, 50, 204), DGX_DARKORCHID(RGB)},
    {"darkred", RGB(139, 0, 0), DGX_DARKRED(RGB)},
    {"darksalmon", RGB(233, 150, 122), DGX_DARKSALMON(RGB)},
    {"darkseagreen", RGB(143, 188, 143), DGX_DARKSEAGREEN(RGB)},
    {"darkslateblue", RGB(72, 61, 139), DGX_DARKSLATEBLUE(RGB)},
    {"darkslategray", RGB(47, 79, 79), DGX_DARKSLATEGRAY(RGB)},
    {"darkslategrey", RGB(47, 79, 79), DGX_DARKSLATEGREY(RGB)},
    {"darkturquoise", RGB(0, 206, 209), DGX_DARKTURQUOISE(RGB)},
    {"darkviolet", RGB(148, 0, 211), DGX_DARKVIOLET(RGB)},
    {"deeppink", RGB(255, 20, 147), DGX_DEEPPINK(RGB)},
    {"deepskyblue", RGB(0, 191, 255), DGX_DEEPSKYBLUE(RGB)},
    {"dimgray", RGB(105, 105, 105), DGX_DIMGRAY(RGB)},
    {"dimgrey", RGB(105, 105, 105), DGX_DIMGREY(RGB)},
    {"dodgerblue", RGB(30, 144, 255), DGX_DODGERBLUE(RGB)},
    {"firebrick", RGB(178, 34, 34), DGX_FIREBRICK(RGB)},
    {"floralwhite", RGB(255, 250, 240), DGX_FLORALWHITE(RGB)},
    {"forestgreen", RGB(34, 139, 34), DGX_FORESTGREEN(RGB)},
    {"fuchsia", RGB(255, 0, 255), DGX_FUCHSIA(RGB)},
    {"gainsboro", RGB(220, 220, 220), DGX_GAINSBORO(RGB)},
    {"ghostwhite", RGB(248, 248, 255), DGX_GHOSTWHITE(RGB)},
    {"gold", RGB(255, 215, 0), DGX_GOLD(RGB)},
    {"goldenrod", RGB(218, 165, 32), DGX_GOLDENROD(RGB)},
    {"gray", RGB(128, 128, 128), DGX_GRAY(RGB)},
    {"green", RGB(0, 128, 0), DGX_GREEN(RGB)},
    {"greenyellow", RGB(173, 255, 47), DGX_GREENYELLOW(RGB)},
    {"grey", RGB(128, 128, 128), DGX_GREY(RGB)},
    {"honeydew", RGB(240, 255, 240), DGX_HONEYDEW(RGB)},
    {"hotpink", RGB(255, 105, 180), DGX_HOTPINK(RGB)},
    {"indianred", RGB(205, 92, 92), DGX_INDIANRED(RGB)},
    {"indigo", RGB(75, 0, 130), DGX_INDIGO(RGB)},
    {"ivory", RGB(255, 255, 240), DGX_IVORY(RGB)},
    {"khaki", RGB(240, 230, 140), DGX_KHAKI(RGB)},
    {"lavender", RGB(230, 230, 250), DGX_LAVENDER(RGB)},
    {"lavenderblush", RGB(255, 240, 245), DGX_LAVENDERBLUSH(RGB)},
    {"lawngreen", RGB(124, 252, 0), DGX_LAWNGREEN(RGB)},
    {"lemonchiffon", RGB(255, 250, 205), DGX_LEMONCHIFFON(RGB)},
    {"lightblue", RGB(173, 216, 230), DGX_LIGHTBLUE(RGB)},
    {"lightcoral", RGB(240, 128, 128), DGX_LIGHTCORAL(RGB)},
    {"lightcyan", RGB(224, 255, 255), DGX_LIGHTCYAN(RGB)},
    {"lightgoldenrodyellow", RGB(250, 250, 210), DGX_LIGHTGOLDENRODYELLOW(RGB)},
    {"lightgray", RGB(211, 211, 211), DGX_LIGHTGRAY(RGB)},
    {"lightgreen", RGB(144, 238, 144), DGX_LIGHTGREEN(RGB)},
    {"lightgrey", RGB(211, 211, 211), DGX_LIGHTGREY(RGB)},
    {"lightpink", RGB(255, 182, 193), DGX_LIGHTPINK(RGB)},
    {"lightsalmon", RGB(255, 160, 122), DGX_LIGHTSALMON(RGB)},
    {"lightseagreen", RGB(32, 178, 170), DGX_LIGHTSEAGREEN(RGB)},
    {"lightskyblue", RGB(135, 206, 250), DGX_LIGHTSKYBLUE(RGB)},
    {"lightslategray", RGB(119, 136, 153), DGX_LIGHTSLATEGRAY(RGB)},
    {"lightslategrey", RGB(119, 136, 153), DGX_LIGHTSLATEGREY(RGB)},
    {"lightsteelblue", RGB(176, 196, 222), DGX_LIGHTSTEELBLUE(RGB)},
    {"lightyellow", RGB(255, 255, 224), DGX_LIGHTYELLOW(RGB)},
    {"lime", RGB(0, 255, 0), DGX_LIME(RGB)},
    {"limegreen", RGB(50, 205, 50), DGX_LIMEGREEN(RGB)},
    {"linen", RGB(250, 240, 230), DGX_LINEN(RGB)},
    {"magenta", RGB(255, 0, 255), DGX_MAGENTA(RGB)},
    {"maroon", RGB(128, 0, 0), DGX_MAROON(RGB)},
    {"mediumaquamarine", RGB(102, 205, 170), DGX_MEDIUMAQUAMARINE(RGB)},
    {"mediumblue", RGB(0, 0, 205), DGX_MEDIUMBLUE(RGB)},
    {"mediumorchid", RGB(186, 85, 211), DGX_MEDIUMORCHID(RGB)},
    {"mediumpurple", RGB(147, 112, 219), DGX_MEDIUMPURPLE(RGB)},
    {"mediumseagreen", RGB(60, 179, 113), DGX_MEDIUMSEAGREEN(RGB)},
    {"mediumslateblue", RGB(123, 104, 238), DGX_MEDIUMSLATEBLUE(RGB)},
    {"mediumspringgreen", RGB(0, 250, 154), DGX_MEDIUMSPRINGGREEN(RGB)},
    {"mediumturquoise", RGB(72, 209, 204), DGX_MEDIUMTURQUOISE(RGB)},
    {"mediumvioletred", RGB(199, 21, 133), DGX_MEDIUMVIOLETRED(RGB)},
    {"midnightblue", RGB(25, 25, 112), DGX_MIDNIGHTBLUE(RGB)},
    {"mintcream", RGB(245, 255, 250), DGX_MINTCREAM(RGB)},
    {"mistyrose", RGB(255, 228, 225), DGX_MISTYROSE(RGB)},
    {"moccasin", RGB(255, 228, 181), DGX_MOCCASIN(RGB)},
    {"navajowhite", RGB(255, 222, 173), DGX_NAVAJOWHITE(RGB)},
    {"navy", RGB(0, 0, 128), DGX_NAVY(RGB)},
    {"oldlace", RGB(253, 245, 230), DGX_OLDLACE(RGB)},
    {"olive", RGB(128, 128, 0), DGX_OLIVE(RGB)},
    {"olivedrab", RGB(107, 142, 35), DGX_OLIVEDRAB(RGB)},
    {"orange", RGB(255, 165, 0), DGX_ORANGE(RGB)},
    {"orangered", RGB(255, 69, 0), DGX_ORANGERED(RGB)},
    {"orchid", RGB(218, 112, 214), DGX_ORCHID(RGB)},
    {"palegoldenrod", RGB(238, 232, 170), DGX_PALEGOLDENROD(RGB)},
    {"palegreen", RGB(152, 251, 152), DGX_PALEGREEN(RGB)},
    {"paleturquoise", RGB(175, 238, 238), DGX_PALETURQUOISE(RGB)},
    {"palevioletred", RGB(219, 112, 147), DGX_PALEVIOLETRED(RGB)},
    {"papayawhip", RGB(255, 239, 213), DGX_PAPAYAWHIP(RGB)},
    {"peachpuff", RGB(255, 218, 185), DGX_PEACHPUFF(RGB)},
    {"peru", RGB(205, 133, 63), DGX_PERU(RGB)},
    {"pink", RGB(255, 192, 203), DGX_PINK(RGB)},
    {"plum", RGB(221, 160, 221), DGX_PLUM(RGB)},
    {"powderblue", RGB(176, 224, 230), DGX_POWDERBLUE(RGB)},
    {"purple", RGB(128, 0, 128), DGX_PURPLE(RGB)},
    {"rebeccapurple", RGB(102, 51, 153), DGX_REBECCAPURPLE(RGB)},
    {"red", RGB(255, 0, 0), DGX_RED(RGB)},
    {"rosybrown", RGB(188, 143, 143), DGX_ROSYBROWN(RGB)},
    {"royalblue", RGB(65, 105, 225), DGX_ROYALBLUE(RGB)},
    {"saddlebrown", RGB(139, 69, 19), DGX_SADDLEBROWN(RGB)},
    {"salmon", RGB(250, 128, 114), DGX_SALMON(RGB)},
    {"sandybrown", RGB(244, 164, 96), DGX_SANDYBROWN(RGB)},
    {"seagreen", RGB(46, 139, 87), DGX_SEAGREEN(RGB)},
    {"seashell", RGB(255, 245, 238), DGX_SEASHELL(RGB)},
    {"sienna", RGB(160, 82, 45), DGX_SIENNA(RGB)},
    {"silver", RGB(192, 192, 192), DGX_SILVER(RGB)},
    {"skyblue", RGB(135, 206, 235), DGX_SKYBLUE(RGB)},
    {"slateblue", RGB(106, 90, 205), DGX_SLATEBLUE(RGB)},
    {"slategray", RGB(112, 128, 144), DGX_SLATEGRAY(RGB)},
    {"slategrey", RGB(112, 128, 144), DGX_SLATEGREY(RGB)},
    {"snow", RGB(255, 250, 250), DGX_SNOW(RGB)},
    {"springgreen", RGB(0, 255, 127), DGX_SPRINGGREEN(RGB)},
    {"steelblue", RGB(70, 130, 180), DGX_STEELBLUE(RGB)},
    {"tan", RGB(210, 180, 140), DGX_TAN(RGB)},
    {"teal", RGB(0, 128, 128), DGX_TEAL(RGB)},
    {"thistle", RGB(216, 191, 216), DGX_THISTLE(RGB)},
    {"tomato", RGB(255, 99, 71), DGX_TOMATO(RGB)},
    {"turquoise", RGB(64, 224, 208), DGX_TURQUOISE(RGB)},
    {"violet", RGB(238, 130, 238), DGX_VIOLET(RGB)},
    {"wheat", RGB(245, 222, 179), DGX_WHEAT(RGB)},
    {"white", RGB(255, 255, 255), DGX_WHITE(RGB)},
    {"whitesmoke", RGB(245, 245, 245), DGX_WHITESMOKE(RGB)},
    {"yellow", RGB(255, 255, 0), DGX_YELLOW(RGB)},
    {"yellowgreen", RGB(154, 205, 50), DGX_YELLOWGREEN(RGB)},
};

int main(void)
{
    const int count = (int)(sizeof names / sizeof *names);
    CHECK(count == 148);
    for (int i = 0; i < count; ++i) {
        if (names[i].dgx != names[i].css) {
            fprintf(stderr, "%s: CSS %06x, DGX %06x\n", names[i].name, (unsigned)names[i].css, (unsigned)names[i].dgx);
            CHECK(false);
        }
    }
    CHECK(DGX_COPPER(RGB) == RGB(184, 115, 51));
    /* the bright green is LIME; the grays from dark to light */
    CHECK(DGX_LIME(RGB) == RGB(0, 255, 0) && DGX_GREEN(RGB) == RGB(0, 128, 0));
    CHECK(DGX_DIMGRAY(RGB) < DGX_GRAY(RGB) && DGX_GRAY(RGB) < DGX_DARKGRAY(RGB) && DGX_DARKGRAY(RGB) == DGX_DARKGREY(RGB) &&
          DGX_DARKGRAY(RGB) < DGX_SILVER(RGB) && DGX_SILVER(RGB) < DGX_LIGHTGREY(RGB) && DGX_LIGHTGREY(RGB) < DGX_GAINSBORO(RGB));
    /* any packer: the packing macros and the inline wrappers */
    CHECK(DGX_MIDNIGHTBLUE(DGX_RGB_16) == dgx_rgb_to_16(25, 25, 112) && DGX_MIDNIGHTBLUE(dgx_rgb_to_16) == DGX_RGB_16(25, 25, 112));
    CHECK(DGX_CORNFLOWERBLUE(DGX_RGB_24) == dgx_rgb_to_24(100, 149, 237));
    CHECK_DONE();
}
