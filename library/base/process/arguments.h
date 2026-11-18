#ifndef PROCESS_ARGUMENTS_H
#define PROCESS_ARGUMENTS_H

#include <vector>

/**
 * @brief The command line of main() as a vector of C strings. The only place that walks argv.
 * @param argc Argument count as received by main().
 * @param argv Argument array as received by main().
 * @return One pointer per argument, `argc` entries. The strings are not copied: they live as long
 *         as `argv`.
 */
std::vector<const char*> commandLine(int argc, char** argv);

#endif
