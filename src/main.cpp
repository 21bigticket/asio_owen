#include <iostream>

#include "app/application.hpp"
#include "common/logger.hpp"

int main(int argc, char* argv[]) {
    try {
        Application app;
        return app.run(argc, argv);
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: ", e.what());
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
