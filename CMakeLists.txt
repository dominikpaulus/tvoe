cmake_minimum_required(VERSION 2.8)

project(tvoe CXX)

add_definitions("-std=c++11 -Wall -D_XOPEN_SOURCE=700 -flto")
set(CMAKE_BUILD_TYPE Debug)

include(FindBISON)
include(FindFLEX)
include(FindPkgConfig)
find_package(BISON)
find_package(FLEX)
find_path(BITSTREAM_INCLUDE_DIR bitstream/common.h)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(EVENT REQUIRED libevent)
pkg_check_modules(EVENT-THREAD REQUIRED libevent_pthreads)
pkg_check_modules(DVBV5 REQUIRED libdvbv5)

BISON_TARGET(ConfigParser config_parser.y
	${CMAKE_CURRENT_BINARY_DIR}/config_parser.cpp)
FLEX_TARGET(ConfigLexer config_lexer.l
	${CMAKE_CURRENT_BINARY_DIR}/config_lexer.cpp)
ADD_FLEX_BISON_DEPENDENCY(ConfigLexer ConfigParser)

INCLUDE_DIRECTORIES($(CMAKE_CURRENT_SOURCE_DIR)
${CMAKE_CURRENT_BINARY_DIR}
${GLIB_INCLUDE_DIRS}
${DVBV5_INCLUDE_DIRS}
${BITSTREAM_INCLUDE_DIR})

ADD_EXECUTABLE(tvoe
	${BISON_ConfigParser_OUTPUTS} ${FLEX_ConfigLexer_OUTPUTS}
	tvoe.cpp http.cpp frontend.cpp log.cpp mpeg.cpp channels.cpp)
TARGET_LINK_LIBRARIES(tvoe
	${EVENT_LIBRARIES} ${EVENT-THREAD_LIBRARIES}
	${GLIB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT}
	${DVBV5_LIBRARIES})
