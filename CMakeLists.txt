cmake_minimum_required (VERSION 2.6)
project (winter)
add_executable (winter winter.c)
install (TARGETS winter DESTINATION bin)
