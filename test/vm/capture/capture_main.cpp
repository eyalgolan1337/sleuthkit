/*
 * capture-ground-truth: Ground truth capture of the validation setup.
 * See test/vm/README.md ("Ground truth capture").
 */
#include "capture.h"

#include <iostream>
#include <string>
#include <vector>

int
main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    return tskval::capture_cli(args, std::cout, std::cerr);
}
