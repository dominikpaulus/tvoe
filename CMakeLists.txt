cmake_minimum_required(VERSION 2.8)

project(tvoe C)

add_definitions("-std=c99 -Wall -D_XOPEN_SOURCE=700 -flto") 
set(CMAKE_BUILD_TYPE Release)

include(FindBISON)
include(FindFLEX)
include(FindPkgConfig)
find_package(BISON)
find_package(FLEX)
find_path(BITSTREAM_INCLUDE_DIR bitstream/common.h)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(EVENT REQUIRED libevent)
pkg_check_modules(EVENT-THREAD REQUIRED libevent_pthreads)

BISON_TARGET(ConfigParser config_parser.y
	${CMAKE_CURRENT_BINARY_DIR}/config_parser.c)
FLEX_TARGET(ConfigLexer config_lexer.l
	${CMAKE_CURRENT_BINARY_DIR}/config_lexer.c)
ADD_FLEX_BISON_DEPENDENCY(ConfigLexer ConfigParser)

INCLUDE_DIRECTORIES($(CMAKE_CURRENT_SOURCE_DIR)
${CMAKE_CURRENT_BINARY_DIR}
${GLIB_INCLUDE_DIRS}
${BITSTREAM_INCLUDE_DIR})

ADD_EXECUTABLE(tvoe
	${BISON_ConfigParser_OUTPUTS} ${FLEX_ConfigLexer_OUTPUTS}
	tvoe.c http.c frontend.c log.c mpeg.c channels.c)
TARGET_LINK_LIBRARIES(tvoe
	${EVENT_LIBRARIES} ${EVENT-THREAD_LIBRARIES}
	${GLIB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
