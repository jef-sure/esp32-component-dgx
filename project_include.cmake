# DGX: the fonts a project needs, generated once for all of them.
#
# ESP-IDF includes this file before the CMakeLists of every component, so the program and any component can
# declare the fonts they need in their own CMakeLists:
#
#     dgx_font_need(text SIZE 12pt MONO TEXTS assets/ru.po assets/pop.pot)   # a face picked by the rules, with the characters of the texts
#     dgx_font_need(hand DGX font0ss TEXTS assets/ru.po EXCEPT "{}")          # one of DGX's fonts, used as is; its characters are checked
#     dgx_font_need(time FILE fonts/PxCgaThin8.c LETTERS "0123456789:")       # a font of the program, used as is
#     dgx_font_need(title SIZE 4.5mm POSTSCRIPT DejaVuSans TEXTS title.txt)  # an exact face at a size in millimeters
#     dgx_font_need(note SOURCE fonts/Note.ttf SIZE 30px TEXTS note.txt)      # a font file of the program, rendered at 30 px
#
# or keep them in a file with the same keywords, dgx_font_needs_file(fonts.needs), which a program's host build can
# read as well (dgx_fonts.py build fonts.needs OUT --scale 4 ...). The program declares, once, the board the sizes
# are for and the rules that pick faces (see font2c/dgx_fonts.py):
#
#     dgx_fonts_board(PIXELS 1280 800 MM 217 136)   # the display, for sizes in millimeters
#     dgx_fonts_scale(4)                             # display pixels per point of the frame, for sizes in points; 1 by default
#     dgx_fonts_rules(fonts.rules)                   # required by any need without an exact face
#
# At the end of the configure step one build rule is created for all the needs: font2c/dgx_fonts.py of DGX generates
# dgx_fonts.c and dgx_fonts.h in the build directory, one font per face and size with the characters of every need
# that uses it, and a function dgx_font_<need>() for each need; the files are compiled into the DGX library and the
# header is on its include path. They are regenerated only when a need's texts, its fonts, the rules or the tool
# change, and rewritten only when they differ. A project with no needs generates nothing and requires nothing of the
# host; one with needs builds font2c from its source on the host (FreeType) and finds faces with fontconfig. Paths
# are resolved from the CMakeLists that gives them.

set(DGX_FONTS_COMPONENT_DIR "${COMPONENT_DIR}")
set(DGX_FONTS_COMPONENT_NAME "${COMPONENT_NAME}")

function(_dgx_font_absolute out path)
    get_filename_component(abs "${path}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    set(${out} "${abs}" PARENT_SCOPE)
endfunction()

# A font a component or the program needs: its name, the characters it must contain (TEXTS, LETTERS) and where it comes from.
function(dgx_font_need name)
    cmake_parse_arguments(PARSE_ARGV 1 N "MONO" "SIZE;FAMILY;POSTSCRIPT;DGX;FILE;SOURCE;LETTERS;EXCEPT" "TEXTS")
    if(NOT name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "dgx_font_need: the name of a need must be a C identifier, not '${name}'")
    endif()
    if(N_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "dgx_font_need(${name}): unknown keywords ${N_UNPARSED_ARGUMENTS}")
    endif()
    foreach(key LETTERS EXCEPT)
        if(DEFINED N_${key} AND N_${key} MATCHES ";")
            message(FATAL_ERROR "dgx_font_need(${name}): a semicolon in ${key}: put such characters into a text file")
        endif()
    endforeach()
    set(lines "need ${name} ${CMAKE_CURRENT_LIST_FILE}")
    set(inputs "")
    if(DEFINED N_SIZE)
        list(APPEND lines "size ${N_SIZE}")
    endif()
    if(N_MONO)
        list(APPEND lines "mono 1")
    endif()
    foreach(key FAMILY POSTSCRIPT LETTERS EXCEPT DGX)
        if(DEFINED N_${key})
            string(TOLOWER "${key}" low)
            list(APPEND lines "${low} ${N_${key}}")
        endif()
    endforeach()
    if(DEFINED N_DGX)
        list(APPEND inputs "${DGX_FONTS_COMPONENT_DIR}/src/fonts/${N_DGX}.c")
    endif()
    foreach(key FILE SOURCE)
        if(DEFINED N_${key})
            string(TOLOWER "${key}" low)
            _dgx_font_absolute(abs "${N_${key}}")
            list(APPEND lines "${low} ${abs}")
            list(APPEND inputs "${abs}")
        endif()
    endforeach()
    foreach(text IN LISTS N_TEXTS)
        _dgx_font_absolute(abs "${text}")
        list(APPEND lines "text ${abs}")
        list(APPEND inputs "${abs}")
    endforeach()
    string(JOIN "\n" record ${lines})
    set_property(GLOBAL APPEND PROPERTY DGX_FONT_NEEDS "${record}")
    set_property(GLOBAL APPEND PROPERTY DGX_FONT_INPUTS ${inputs})
endfunction()

# The display the fonts are made for: PIXELS w h and MM w h (for sizes in mm). Declared once, by the program or its board file.
function(dgx_fonts_board)
    cmake_parse_arguments(PARSE_ARGV 0 B "" "" "PIXELS;MM")
    if(B_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "dgx_fonts_board: unknown keywords ${B_UNPARSED_ARGUMENTS}; expected PIXELS w h, MM w h (the scale is declared with dgx_fonts_scale(n))")
    endif()
    set(lines "")
    foreach(key PIXELS MM)
        if(DEFINED B_${key})
            list(LENGTH B_${key} count)
            if(NOT count EQUAL 2)
                message(FATAL_ERROR "dgx_fonts_board: ${key} takes the width and the height")
            endif()
            string(TOLOWER "${key}" low)
            string(REPLACE ";" " " pair "${B_${key}}")
            list(APPEND lines "board ${low} ${pair}")
        endif()
    endforeach()
    if(NOT lines)
        message(FATAL_ERROR "dgx_fonts_board: expected PIXELS w h, MM w h")
    endif()
    set_property(GLOBAL APPEND PROPERTY DGX_FONTS_SETTINGS ${lines})
endfunction()

# The frame scale: display pixels per point of the frame, for sizes in points; 1 by default. Declared once, by the program.
function(dgx_fonts_scale n)
    if(NOT n MATCHES "^[0-9]+(\\.[0-9]+)?$")
        message(FATAL_ERROR "dgx_fonts_scale: expected a number, not '${n}'")
    endif()
    set_property(GLOBAL APPEND PROPERTY DGX_FONTS_SETTINGS "scale ${n}")
endfunction()

# The rules that pick faces for needs without an exact face (the file format: font2c/dgx_fonts.py). Declared once.
function(dgx_fonts_rules path)
    get_property(told GLOBAL PROPERTY DGX_FONTS_RULES)
    if(told)
        message(FATAL_ERROR "dgx_fonts_rules: declared twice")
    endif()
    _dgx_font_absolute(abs "${path}")
    set_property(GLOBAL PROPERTY DGX_FONTS_RULES "${abs}")
endfunction()

# The needs of a file, one keyword per line, the same keywords dgx_font_need() takes (the format is described in
# font2c/dgx_fonts.py): for a program that is also built on the host, where dgx_fonts.py generates the same fonts
# from the same file without CMake. Paths in the file are resolved from its directory; rules, scale and board lines
# in it count as dgx_fonts_rules(), dgx_fonts_scale() and dgx_fonts_board().
function(dgx_font_needs_file path)
    _dgx_font_absolute(abs "${path}")
    if(NOT EXISTS "${abs}")
        message(FATAL_ERROR "dgx_font_needs_file: ${abs} does not exist")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${abs}")
    get_filename_component(dir "${abs}" DIRECTORY)
    file(STRINGS "${abs}" lines ENCODING UTF-8)
    set(record "")
    set(inputs "")
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(STRIP "${line}" line)
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        if(line MATCHES "^need[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
            if(record)
                set_property(GLOBAL APPEND PROPERTY DGX_FONT_NEEDS "${record}")
            endif()
            set(record "need ${CMAKE_MATCH_1} ${abs}:${number}")
        elseif(line MATCHES "^(text|file|source)[ \t]+(.+)$")
            set(key "${CMAKE_MATCH_1}")
            get_filename_component(p "${CMAKE_MATCH_2}" ABSOLUTE BASE_DIR "${dir}")
            string(APPEND record "\n${key} ${p}")
            list(APPEND inputs "${p}")
        elseif(line MATCHES "^dgx[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
            string(APPEND record "\ndgx ${CMAKE_MATCH_1}")
            list(APPEND inputs "${DGX_FONTS_COMPONENT_DIR}/src/fonts/${CMAKE_MATCH_1}.c")
        elseif(line MATCHES "^rules[ \t]+(.+)$")
            get_property(told GLOBAL PROPERTY DGX_FONTS_RULES)
            if(told)
                message(FATAL_ERROR "${abs}:${number}: the rules are declared twice")
            endif()
            get_filename_component(p "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${dir}")
            set_property(GLOBAL PROPERTY DGX_FONTS_RULES "${p}")
        elseif(line MATCHES "^board[ \t]+(pixels|mm)[ \t]+" OR line MATCHES "^scale[ \t]+")
            set_property(GLOBAL APPEND PROPERTY DGX_FONTS_SETTINGS "${line}")
        elseif(line MATCHES "^(size|mono|family|postscript|letters|except)[ \t]")
            if(NOT record)
                message(FATAL_ERROR "${abs}:${number}: '${line}' comes before any need")
            endif()
            string(APPEND record "\n${line}")
        else()
            message(FATAL_ERROR "${abs}:${number}: not a keyword of a need: '${line}'")
        endif()
    endforeach()
    if(record)
        set_property(GLOBAL APPEND PROPERTY DGX_FONT_NEEDS "${record}")
    endif()
    set_property(GLOBAL APPEND PROPERTY DGX_FONT_INPUTS ${inputs})
endfunction()

# A list of fonts for font2c -jo, passed through as is: one font per line, [-n name] [-f letters] fontfile size
# [first last]..., paths resolved from the directory of the list. Its fonts go into the same dgx_fonts.c and their
# functions are declared in dgx_fonts.h: for a component or a program that names its font files and characters
# itself, alongside the needs.
function(dgx_font_list path)
    _dgx_font_absolute(abs "${path}")
    if(NOT EXISTS "${abs}")
        message(FATAL_ERROR "dgx_font_list: ${abs} does not exist")
    endif()
    get_filename_component(dir "${abs}" DIRECTORY)
    set(inputs "${abs}")
    file(STRINGS "${abs}" lines ENCODING UTF-8)
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "#.*$" "" line "${line}")
        separate_arguments(words NATIVE_COMMAND "${line}")
        foreach(word IN LISTS words)
            if(word MATCHES "\\.(ttf|otf|ttc|json|txt)$")
                get_filename_component(p "${word}" ABSOLUTE BASE_DIR "${dir}")
                list(APPEND inputs "${p}")
            endif()
        endforeach()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY DGX_FONT_NEEDS "list ${abs}")
    set_property(GLOBAL APPEND PROPERTY DGX_FONT_INPUTS ${inputs})
endfunction()

# The single build rule for all the needs, created at the end of the configure step.
function(_dgx_fonts_finish)
    get_property(needs GLOBAL PROPERTY DGX_FONT_NEEDS)
    if(NOT needs)
        return()
    endif()
    get_property(board GLOBAL PROPERTY DGX_FONTS_SETTINGS)
    get_property(rules GLOBAL PROPERTY DGX_FONTS_RULES)
    get_property(inputs GLOBAL PROPERTY DGX_FONT_INPUTS)
    set(out "${CMAKE_BINARY_DIR}/dgx_fonts")
    file(MAKE_DIRECTORY "${out}")
    set(content "# the needs of the project, gathered from its CMakeLists files at configure time; generated, do not edit\ncomponent ${DGX_FONTS_COMPONENT_DIR}\n")
    foreach(line IN LISTS board)
        string(APPEND content "${line}\n")
    endforeach()
    if(rules)
        string(APPEND content "rules ${rules}\n")
        list(APPEND inputs "${rules}")
    endif()
    string(JOIN "\n" all ${needs})
    string(APPEND content "${all}\n")
    set(needs_file "${out}/needs.txt")
    set(old "")
    if(EXISTS "${needs_file}")
        file(READ "${needs_file}" old)
    endif()
    if(NOT old STREQUAL content)
        file(WRITE "${needs_file}" "${content}")
    endif()
    idf_build_get_property(python PYTHON)
    set(tool "${DGX_FONTS_COMPONENT_DIR}/font2c/dgx_fonts.py")
    add_custom_command(OUTPUT "${out}/dgx_fonts.c" "${out}/dgx_fonts.h"
        COMMAND ${python} "${tool}" build "${needs_file}" "${out}"
        DEPENDS "${tool}" "${DGX_FONTS_COMPONENT_DIR}/font2c/font2c.c" "${needs_file}" ${inputs}
        COMMENT "DGX: the fonts of the program by its needs"
        VERBATIM)
    add_custom_target(dgx_fonts_made DEPENDS "${out}/dgx_fonts.c" "${out}/dgx_fonts.h")
    idf_component_get_property(lib ${DGX_FONTS_COMPONENT_NAME} COMPONENT_LIB)
    set_source_files_properties("${out}/dgx_fonts.c" "${out}/dgx_fonts.h" TARGET_DIRECTORY ${lib} PROPERTIES GENERATED TRUE)
    target_sources(${lib} PRIVATE "${out}/dgx_fonts.c")
    target_include_directories(${lib} PUBLIC "${out}")
    add_dependencies(${lib} dgx_fonts_made)
    set(count 0)
    set(listed 0)
    foreach(n IN LISTS needs)
        if(n MATCHES "^list ")
            math(EXPR listed "${listed} + 1")
        else()
            math(EXPR count "${count} + 1")
        endif()
    endforeach()
    message(STATUS "DGX fonts: ${count} need(s) and ${listed} list(s), generated into ${out}")
endfunction()

cmake_language(DEFER CALL _dgx_fonts_finish)
