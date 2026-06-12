#include <cassert>
#include <iostream>

#include "chronosx/libxdp_socket.hpp"

int main() {
#ifdef CHRONOSX_ENABLE_LIBXDP
    chronosx::LibxdpSocket socket;
    assert(socket.umem_area() == nullptr);
    assert(socket.umem_size_bytes() == 0);
    assert(socket.kick_tx() == chronosx::SocketStatus::NotInitialized);
    std::cout << "libxdp socket compile smoke passed\n";
#else
    std::cout << "libxdp disabled\n";
#endif
    return 0;
}
