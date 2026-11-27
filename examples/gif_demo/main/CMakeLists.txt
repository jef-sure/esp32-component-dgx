idf_component_register(SRCS "main.c")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
# the three GIF animations, a need a line: each made into a .dgxi file of its frames and embedded as dgx_asset_<name>
dgx_image_needs_file(images.txt)
