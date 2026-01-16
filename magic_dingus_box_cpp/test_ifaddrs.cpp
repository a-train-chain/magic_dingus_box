#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <iostream>
#include <string>

int main() {
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == -1) {
        perror("getifaddrs");
        return 1;
    }

    bool usb0_found = false;
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        std::string name = ifa->ifa_name;
        // Check for IPv4 addresses only
        if (ifa->ifa_addr->sa_family == AF_INET) {
            std::cout << "Interface: " << name << " (UP)" << std::endl;
            if (name == "usb0") usb0_found = true;
        }
    }

    freeifaddrs(ifap);
    
    if (usb0_found) {
        std::cout << "usb0 is active!" << std::endl;
    } else {
        std::cout << "usb0 not found or not IP4." << std::endl;
    }
    return 0;
}
