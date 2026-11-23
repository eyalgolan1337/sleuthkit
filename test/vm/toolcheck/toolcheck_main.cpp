/*
 * check-tool: the CLI tool checks' command. See toolcheck_cli.h.
 */
#include "toolcheck_cli.h"

#include <iostream>
#include <string>
#include <vector>

int
main(int argc, char **argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    return tskval::check_tool_cli(args, std::cout, std::cerr);
}
