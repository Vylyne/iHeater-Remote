#pragma once
#include <Arduino.h>

// iHeater Remote owns all of its release version components.
#define VERSION_MAJOR 2
#define VERSION_MINOR 0
#define VERSION_PATCH 0

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define VERSION_STR                                                            \
  STR(VERSION_MAJOR) "." STR(VERSION_MINOR) "." STR(VERSION_PATCH)
#define VERSION_STRING VERSION_STR
#define VERSION_NUMBER                                                         \
  ((VERSION_MAJOR << 16) | (VERSION_MINOR << 8) | VERSION_PATCH)
