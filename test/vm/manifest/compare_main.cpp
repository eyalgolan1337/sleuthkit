/*
 * compare-manifests: the comparison step of the validation setup.
 * See test/vm/README.md ("The comparison step").
 */
#include "compare_cli.h"

#include <iostream>
#include <string>
#include <vector>

int
main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    return tskval::compare_cli(args, std::cout, std::cerr);
}
