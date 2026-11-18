#include "arguments.h"

std::vector<const char*> commandLine(int argc, char** argv) {
    return std::vector<const char*>(argv, argv + argc);
}
