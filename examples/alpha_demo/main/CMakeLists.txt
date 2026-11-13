idf_component_register(SRCS "main.c")

# the word of the soft shape, DejaVu Sans of 4 bits a pixel, drawn once into an 8-bit screen of amounts; its letters
# in another order, as "DGX" alone would be taken for the keyword DGX of dgx_font_need()
dgx_font_need(word SIZE 22px POSTSCRIPT DejaVuSans BITS 4 LETTERS "GXD")
