/*
 * tsk-harness: writes a Manifest of a file system in a disk image through
 * the public libtsk API. See test/vm/README.md ("The harness").
 */
#include "harness.h"

#include <iostream>
#include <string>
#include <vector>

int
main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    return tskval::harness_cli(args, std::cout, std::cerr);
}
