#pragma once
#include <string>
#include <array>
#include <cstdio>

// Query the ARP/neighbor table for the given IP.
// Returns MAC address like "aa:bb:cc:dd:ee:ff" if available, else empty.
inline std::string lookup_mac_for_ip(const std::string& ip) {
    std::array<char, 512> buf{};
    std::string out;

    // Run "ip neigh show <ip>"
    std::string cmd = "ip neigh show " + ip + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    while (fgets(buf.data(), buf.size(), p)) {
        out += buf.data();
    }
    pclose(p);

    // Typical output: "192.168.1.42 dev eth0 lladdr aa:bb:cc:dd:ee:ff REACHABLE"
    auto pos = out.find("lladdr ");
    if (pos == std::string::npos) return {};
    pos += 7; // after "lladdr "
    auto end = out.find(' ', pos);
    if (end == std::string::npos) end = out.size();
    return out.substr(pos, end - pos);
}
