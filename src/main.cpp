#include <iostream>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

void print_task()
{
    std::cout << "run task\n";
}

int main()
{
    std::cout << "ApiGate bootstrap OK\n";

    boost::asio::io_context io;
    boost::asio::post(io, print_task);
    std::cout << io.run() << std::endl;

    return 0;
}
