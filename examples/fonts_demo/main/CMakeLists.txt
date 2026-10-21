idf_component_register(SRCS "main.c")

# the CYD: a 320x240 ILI9341 of 57.6x43.2 mm, used in landscape
dgx_fonts_board(PIXELS 320 240 MM 57.6 43.2)
# which faces may be used at which sizes, from the fonts installed on the build machine
dgx_fonts_rules(../fonts.rules)
# a monospaced face picked by the rules, 2.5 mm high, with the characters of the paragraph
dgx_font_need(body SIZE 2.5mm MONO TEXTS strings.txt)
# an exact face, 4.5 mm high, with the characters of the title
dgx_font_need(title SIZE 4.5mm POSTSCRIPT DejaVuSans-Bold TEXTS title.txt)
# a font bundled with DGX, used as is; the build checks that it has the digits and the colon
dgx_font_need(clock DGX VerdanaRegular32 LETTERS "0123456789:")
