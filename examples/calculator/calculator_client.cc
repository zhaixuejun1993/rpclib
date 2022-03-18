#include <iostream>

#include "rpc/client.h"
#include "rpc/rpc_error.h"

int main() {
    rpc::client c("localhost", 8080);

    try {
        std::cout << "add(2, 3) = ";
        double five = c.call("add", 2, 3).get().as<double>();
        std::cout << five << std::endl;

        std::cout << "sub(3, 2) = ";
        double one = c.call("sub", 3, 2).get().as<double>();
        std::cout << one << std::endl;

        std::cout << "mul(5, 0) = ";
        double zero = c.call("mul", five, 0).get().as<double>();
        std::cout << zero << std::endl;

        std::cout << "client send msg(msg from client)";
        c.call("printInfo", "test msg from client");
        std::cout << "done" << std::endl;
    } catch (rpc::rpc_error &e) {
        std::cout << std::endl << e.what() << std::endl;
        std::cout << "in function '" << e.get_function_name() << "': ";

        using err_t = std::tuple<int, std::string>;
        auto err = e.get_error().get().as<err_t>();
        std::cout << "[error " << std::get<0>(err) << "]: " << std::get<1>(err)
                  << std::endl;
        return 1;
    }

    return 0;
}
