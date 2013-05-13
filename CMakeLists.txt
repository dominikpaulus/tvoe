cmake_minimum_required(VERSION 2.8)
include(FindBISON)
include(FindFLEX)

find_package(BISON)
find_package(FLEX)

find_path(GLIB_INCLUDE_DIR NAMES glib.h PATH_SUFFIXES glib-2.0)

BISON_TARGET(ConfigParser config_parser.y ${CMAKE_CURRENT_BINARY_DIR}/config_parser.c)
FLEX_TARGET(ConfigLexer config_lexer.l ${CMAKE_CURRENT_BINARY_DIR}/config_lexer.c)
ADD_FLEX_BISON_DEPENDENCY(ConfigLexer ConfigParser)

INCLUDE_DIRECTORIES($(CMAKE_CURRENT_SOURCE_DIR)
${CMAKE_CURRENT_BINARY_DIR},
${GLIB_INCLUDE_DIR}
/usr/lib/glib-2.0/include) # FIXME

ADD_EXECUTABLE(getstream ${BISON_ConfigParser_OUTPUTS}
	${FLEX_ConfigLexer_OUTPUTS} getstream.c http.c frontend.c log.c)
TARGET_LINK_LIBRARIES(getstream event glib-2.0) # TODO
