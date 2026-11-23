/*
 * tsk-corpus: the Test corpus generator of the validation setup.
 * See test/vm/README.md ("The Test corpus").
 */
#include "corpus.h"

#include <iostream>
#include <string>
#include <vector>

int
main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    return tskval::corpus_cli(args, std::cout, std::cerr);
}
