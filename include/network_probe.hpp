#pragma once

#include <string>
#include <vector>

namespace xcloud {

struct NetworkProbeCheck {
    std::string name;
    int result = 0;
    int errorCode = 0;
    bool passed = false;
};

bool runNetworkProbe(std::vector<NetworkProbeCheck>& checks);

} // namespace xcloud
