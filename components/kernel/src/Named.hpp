#pragma once

namespace cornucopia::ugly_duckling::kernel {

class Named {
protected:
    Named(const std::string& name)
        : name(name) {
    }

public:
    const std::string name;
};

}    // namespace cornucopia::ugly_duckling::kernel
