/*
 * Runs every CppUnit fixture registered in the fs layer's unit tests, or,
 * with an argument, the suites whose names contain it (XfsCorruptTest).
 */
#include <cppunit/CompilerOutputter.h>
#include <cppunit/Test.h>
#include <cppunit/TestResult.h>
#include <cppunit/TestResultCollector.h>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TestRunner.h>

#include <iostream>
#include <memory>
#include <string>

int
main(int argc, char **argv)
{
    if (argc > 1) {
        const std::unique_ptr<CppUnit::Test> all(
            CppUnit::TestFactoryRegistry::getRegistry().makeTest());
        CppUnit::TestResult controller;
        CppUnit::TestResultCollector result;
        controller.addListener(&result);
        bool found = false;
        for (int i = 0; i < all->getChildTestCount(); ++i) {
            CppUnit::Test *suite = all->getChildTestAt(i);
            if (suite->getName().find(argv[1]) != std::string::npos) {
                found = true;
                suite->run(&controller);
            }
        }
        if (!found) {
            std::cerr << "no suite named " << argv[1] << "\n";
            return 1;
        }
        CppUnit::CompilerOutputter(&result, std::cerr).write();
        return result.wasSuccessful() ? 0 : 1;
    }
    CppUnit::TextUi::TestRunner runner;
    runner.addTest(CppUnit::TestFactoryRegistry::getRegistry().makeTest());
    runner.setOutputter(
        new CppUnit::CompilerOutputter(&runner.result(), std::cerr));
    return runner.run() ? 0 : 1;
}
