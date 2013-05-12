cmake_minimum_required(VERSION 2.8)
include(FindBISON)
include(FindFLEX)

find_package(BISON)
find_package(FLEX)

BISON_TARGET(ConfigParser config_parser.y ${CMAKE_CURRENT_BINARY_DIR}/config_parser.c)
FLEX_TARGET(ConfigLexer config_lexer.l ${CMAKE_CURRENT_BINARY_DIR}/config_lexer.c)
ADD_FLEX_BISON_DEPENDENCY(ConfigLexer ConfigParser)

INCLUDE_DIRECTORIES($(CMAKE_CURRENT_SOURCE_DIR)
${CMAKE_CURRENT_BINARY_DIR})

ADD_EXECUTABLE(getstream ${BISON_ConfigParser_OUTPUTS}
	${FLEX_ConfigLexer_OUTPUTS} getstream.c)
