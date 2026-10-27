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
#     dgx_font_need(menu SIZE 9px FAMILY "Droid Sans" PATCH fixes/menu.patch.json TEXTS menu.po)  # with the fixes of the font editor
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
# are resolved from the CMakeLists that gives them. Beside the two files, fonts-used.txt lists every font the program
# uses, with what its file says of its license; a deny in the rules keeps a font out however it is asked for.
#
# A font rendered from a face takes its patch, the fixes made to it in the font editor of DGX: the one PATCH names,
# or else <name>.patch.json, <name> being the name font2c gives the font (family, style and size), found beside the
# CMakeLists that declares the need, beside its SOURCE, or in src/fonts of DGX. A patch added or changed there makes
# the fonts again.

set(DGX_FONTS_COMPONENT_DIR "${COMPONENT_DIR}")
set(DGX_FONTS_COMPONENT_NAME "${COMPONENT_NAME}")

# cmake_language(DEFER) gathers the needs at the end of the configure step; it came with CMake 3.19. The CMake of
# ESP-IDF's tools is newer; an older one of the system stops at the first need with this message, and a project
# without needs is not affected.
if(CMAKE_VERSION VERSION_LESS 3.19)
    set(DGX_FONTS_TOO_OLD "the fonts made at build time need CMake 3.19 or later, and this is CMake ${CMAKE_VERSION}")
endif()

# A record of a need waits in a CMake list until it is written: the brackets and semicolons among its characters are
# hidden on the way, or CMake would split and join the list at them.
function(_dgx_fonts_encode out text)
    string(REPLACE "[" "@DGX_LB@" text "${text}")
    string(REPLACE "]" "@DGX_RB@" text "${text}")
    string(REPLACE ";" "@DGX_SEMI@" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(_dgx_fonts_decode out text)
    string(REPLACE "@DGX_LB@" "[" text "${text}")
    string(REPLACE "@DGX_RB@" "]" text "${text}")
    string(REPLACE "@DGX_SEMI@" ";" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# the patches of the font editor in a directory; one added or removed there configures the project again
function(_dgx_fonts_patches_in out dir)
    file(GLOB found CONFIGURE_DEPENDS "${dir}/*.patch.json")
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

function(_dgx_font_absolute out path)
    get_filename_component(abs "${path}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    set(${out} "${abs}" PARENT_SCOPE)
endfunction()

# A font a component or the program needs: its name, the characters it must contain (TEXTS, LETTERS) and where it comes from.
function(dgx_font_need name)
    if(DGX_FONTS_TOO_OLD)
        message(FATAL_ERROR "dgx_font_need(${name}): ${DGX_FONTS_TOO_OLD}")
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 N "MONO" "SIZE;FAMILY;POSTSCRIPT;DGX;FILE;SOURCE;LETTERS;EXCEPT;PATCH" "TEXTS")
    if(NOT name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "dgx_font_need: the name of a need must be a C identifier, not '${name}'")
    endif()
    if(N_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "dgx_font_need(${name}): unknown keywords ${N_UNPARSED_ARGUMENTS}")
    endif()
    # a keyword given twice would keep its last value in silence
    if(ARGC GREATER 1)
        math(EXPR last "${ARGC} - 1")
        foreach(key SIZE MONO FAMILY POSTSCRIPT DGX FILE SOURCE LETTERS EXCEPT PATCH)
            set(times 0)
            foreach(i RANGE 1 ${last})
                if("${ARGV${i}}" STREQUAL "${key}")
                    math(EXPR times "${times} + 1")
                endif()
            endforeach()
            if(times GREATER 1)
                message(FATAL_ERROR "dgx_font_need(${name}): ${key} given twice")
            endif()
        endforeach()
    endif()
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
            _dgx_fonts_encode(value "${N_${key}}")
            list(APPEND lines "${low} ${value}")
        endif()
    endforeach()
    if(DEFINED N_DGX)
        if(NOT EXISTS "${DGX_FONTS_COMPONENT_DIR}/src/fonts/${N_DGX}.c")
            message(FATAL_ERROR "dgx_font_need(${name}): DGX has no font ${N_DGX}; its fonts are the .c files of ${DGX_FONTS_COMPONENT_DIR}/src/fonts")
        endif()
        list(APPEND inputs "${DGX_FONTS_COMPONENT_DIR}/src/fonts/${N_DGX}.c")
    endif()
    foreach(key FILE SOURCE PATCH)
        if(DEFINED N_${key})
            string(TOLOWER "${key}" low)
            _dgx_font_absolute(abs "${N_${key}}")
            if(NOT EXISTS "${abs}")
                message(FATAL_ERROR "dgx_font_need(${name}): ${key} ${abs} does not exist")
            endif()
            list(APPEND lines "${low} ${abs}")
            list(APPEND inputs "${abs}")
        endif()
    endforeach()
    # the patches its font may find by its name: beside this CMakeLists and beside its SOURCE
    _dgx_fonts_patches_in(patches "${CMAKE_CURRENT_LIST_DIR}")
    list(APPEND inputs ${patches})
    if(DEFINED N_SOURCE)
        _dgx_font_absolute(abs "${N_SOURCE}")
        get_filename_component(dir "${abs}" DIRECTORY)
        _dgx_fonts_patches_in(patches "${dir}")
        list(APPEND inputs ${patches})
    endif()
    foreach(text IN LISTS N_TEXTS)
        _dgx_font_absolute(abs "${text}")
        if(NOT EXISTS "${abs}")
            message(FATAL_ERROR "dgx_font_need(${name}): TEXTS ${abs} does not exist")
        endif()
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
    if(DGX_FONTS_TOO_OLD)
        message(FATAL_ERROR "dgx_font_needs_file(${path}): ${DGX_FONTS_TOO_OLD}")
    endif()
    if(NOT EXISTS "${abs}")
        message(FATAL_ERROR "dgx_font_needs_file: ${abs} does not exist")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${abs}")
    get_filename_component(dir "${abs}" DIRECTORY)
    # read whole and split here: a bracket among the characters of a line would make CMake join the lines of a list
    file(READ "${abs}" content)
    string(ASCII 239 187 191 bom)
    string(SUBSTRING "${content}" 0 3 head)
    if(head STREQUAL bom)
        string(SUBSTRING "${content}" 3 -1 content)
    endif()
    string(REPLACE "\r" "" content "${content}")
    _dgx_fonts_encode(content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(record "")
    set(inputs "")
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "^[ \t]+" "" line "${line}")
        if(line MATCHES "^[ \t]*$" OR line MATCHES "^#")
            continue()
        endif()
        if(line MATCHES "^need[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
            if(record)
                set_property(GLOBAL APPEND PROPERTY DGX_FONT_NEEDS "${record}")
            endif()
            set(record "need ${CMAKE_MATCH_1} ${abs}:${number}")
        elseif(line MATCHES "^(text|file|source|patch)[ \t]+(.+)$")
            set(key "${CMAKE_MATCH_1}")
            string(STRIP "${CMAKE_MATCH_2}" given)
            _dgx_fonts_decode(given "${given}")
            get_filename_component(p "${given}" ABSOLUTE BASE_DIR "${dir}")
            if(NOT EXISTS "${p}")
                message(FATAL_ERROR "${abs}:${number}: ${key} ${p} does not exist")
            endif()
            if(key STREQUAL "source")
                get_filename_component(sdir "${p}" DIRECTORY)
                _dgx_fonts_patches_in(patches "${sdir}")
                list(APPEND inputs ${patches})
            endif()
            _dgx_fonts_encode(p "${p}")
            string(APPEND record "\n${key} ${p}")
            list(APPEND inputs "${p}")
        elseif(line MATCHES "^dgx[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
            if(NOT EXISTS "${DGX_FONTS_COMPONENT_DIR}/src/fonts/${CMAKE_MATCH_1}.c")
                message(FATAL_ERROR "${abs}:${number}: DGX has no font ${CMAKE_MATCH_1}; its fonts are the .c files of ${DGX_FONTS_COMPONENT_DIR}/src/fonts")
            endif()
            string(APPEND record "\ndgx ${CMAKE_MATCH_1}")
            list(APPEND inputs "${DGX_FONTS_COMPONENT_DIR}/src/fonts/${CMAKE_MATCH_1}.c")
        elseif(line MATCHES "^rules[ \t]+(.+)$")
            get_property(told GLOBAL PROPERTY DGX_FONTS_RULES)
            if(told)
                message(FATAL_ERROR "${abs}:${number}: the rules are declared twice")
            endif()
            string(STRIP "${CMAKE_MATCH_1}" given)
            _dgx_fonts_decode(given "${given}")
            get_filename_component(p "${given}" ABSOLUTE BASE_DIR "${dir}")
            set_property(GLOBAL PROPERTY DGX_FONTS_RULES "${p}")
        elseif(line MATCHES "^board[ \t]+(pixels|mm)[ \t]+" OR line MATCHES "^scale[ \t]+")
            string(STRIP "${line}" line)
            set_property(GLOBAL APPEND PROPERTY DGX_FONTS_SETTINGS "${line}")
        elseif(line MATCHES "^(letters|except)[ \t]")
            if(NOT record)
                message(FATAL_ERROR "${abs}:${number}: '${line}' comes before any need")
            endif()
            string(APPEND record "\n${line}")
        elseif(line MATCHES "^(size|mono|family|postscript)[ \t]")
            if(NOT record)
                message(FATAL_ERROR "${abs}:${number}: '${line}' comes before any need")
            endif()
            string(STRIP "${line}" line)
            string(APPEND record "\n${line}")
        else()
            _dgx_fonts_decode(line "${line}")
            message(FATAL_ERROR "${abs}:${number}: not a keyword of a need: '${line}'")
        endif()
    endforeach()
    if(record)
        set_property(GLOBAL APPEND PROPERTY DGX_FONT_NEEDS "${record}")
    endif()
    # the patches its fonts may find by their names beside it
    _dgx_fonts_patches_in(patches "${dir}")
    list(APPEND inputs ${patches})
    set_property(GLOBAL APPEND PROPERTY DGX_FONT_INPUTS ${inputs})
endfunction()

# A list of fonts for font2c -jo, passed through as is: one font per line, [-n name] [-f letters] [-p patch]
# [-P dir] fontfile size [first last]..., paths resolved from the directory of the list. Its fonts go into the same dgx_fonts.c and their
# functions are declared in dgx_fonts.h: for a component or a program that names its font files and characters
# itself, alongside the needs.
function(dgx_font_list path)
    _dgx_font_absolute(abs "${path}")
    if(DGX_FONTS_TOO_OLD)
        message(FATAL_ERROR "dgx_font_list(${path}): ${DGX_FONTS_TOO_OLD}")
    endif()
    if(NOT EXISTS "${abs}")
        message(FATAL_ERROR "dgx_font_list: ${abs} does not exist")
    endif()
    get_filename_component(dir "${abs}" DIRECTORY)
    set(inputs "${abs}")
    file(STRINGS "${abs}" lines ENCODING UTF-8)
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "#.*$" "" line "${line}")
        separate_arguments(words NATIVE_COMMAND "${line}")
        # every word that names a file, whatever its ending: the font files and the files of -f
        foreach(word IN LISTS words)
            if(word MATCHES "^-" OR word MATCHES "^(0[xX][0-9A-Fa-f]+|[0-9]+)$")
                continue()
            endif()
            get_filename_component(p "${word}" ABSOLUTE BASE_DIR "${dir}")
            if(EXISTS "${p}" AND NOT IS_DIRECTORY "${p}")
                list(APPEND inputs "${p}")
            elseif(IS_DIRECTORY "${p}")
                # a directory of -P: the patches font2c may find in it
                _dgx_fonts_patches_in(patches "${p}")
                list(APPEND inputs ${patches})
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
    # the fixes DGX keeps for the faces it knows, taken by every font made of one of them
    _dgx_fonts_patches_in(patches "${DGX_FONTS_COMPONENT_DIR}/src/fonts")
    list(APPEND inputs ${patches})
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
    _dgx_fonts_decode(all "${all}")
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
    add_custom_command(OUTPUT "${out}/dgx_fonts.c" "${out}/dgx_fonts.h" "${out}/fonts-used.txt"
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
    message(STATUS "DGX fonts: ${count} need(s) and ${listed} list(s), generated into ${out}; "
        "the fonts used and their licenses are listed in ${out}/fonts-used.txt: make sure you may use every one of them")
endfunction()

if(NOT DGX_FONTS_TOO_OLD)
    cmake_language(DEFER CALL _dgx_fonts_finish)
endif()
