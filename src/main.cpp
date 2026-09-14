#include <iostream>
#include <boost/asio/io_context.hpp>

int main()
{
    std::cout << "ApiGate bootstrap OK\n";

    boost::asio::io_context io;
    std::cout << io.run() << std::endl;

    return 0;
}
