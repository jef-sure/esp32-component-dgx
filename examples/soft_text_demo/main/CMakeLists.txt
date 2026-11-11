idf_component_register(SRCS "main.c")

# the CYD: a 320x240 ILI9341 of 57.6x43.2 mm, used in landscape
dgx_fonts_board(PIXELS 320 240 MM 57.6 43.2)
# DejaVu Sans at 16 and 12 px, of 4 bits a pixel and of one, with the characters of the lines
dgx_font_need(soft SIZE 16px POSTSCRIPT DejaVuSans BITS 4 TEXTS strings.txt)
dgx_font_need(hard SIZE 16px POSTSCRIPT DejaVuSans TEXTS strings.txt)
dgx_font_need(small SIZE 12px POSTSCRIPT DejaVuSans BITS 4 TEXTS strings.txt)
dgx_font_need(small_hard SIZE 12px POSTSCRIPT DejaVuSans TEXTS strings.txt)
# the counter, drawn again over itself ten times a second
dgx_font_need(count SIZE 36px POSTSCRIPT DejaVuSans BITS 4 LETTERS "0123456789.")
