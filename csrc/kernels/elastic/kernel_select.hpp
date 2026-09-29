#pragma once

// MIT License
//
// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <deep_ep/common/exception.cuh>

namespace deep_ep::elastic {

namespace detail {

enum class RdmaNicType { NonEfa, Efa, Unknown };

inline RdmaNicType detect_rdma_nic_type(const std::filesystem::path& root) {
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(root, ec);
    if (ec)
        return RdmaNicType::Unknown;

    bool found = false, unknown = false;
    for (const auto end = std::filesystem::directory_iterator(); it != end; it.increment(ec)) {
        if (ec)
            return RdmaNicType::Unknown;
        found = true;
        const auto device = it->path() / "device";
        const auto driver_path = std::filesystem::read_symlink(device / "driver", ec);
        std::string driver = ec ? "" : driver_path.filename().string();
        ec.clear();
        if (driver.empty()) {
            // Some containers expose uevent but not the driver symlink.
            std::ifstream uevent(device / "uevent");
            for (std::string line; std::getline(uevent, line);) {
                if (line.compare(0, 7, "DRIVER=") == 0) {
                    driver = line.substr(7);
                    break;
                }
            }
        }
        if (driver == "efa")
            return RdmaNicType::Efa;
        unknown |= driver.empty();
    }
    return ec or not found or unknown ? RdmaNicType::Unknown : RdmaNicType::NonEfa;
}

inline bool resolve_ordered_hybrid_kernel(const std::string& value, const std::filesystem::path& root) {
    if (value == "ordered")
        return true;
    if (value == "unordered")
        return false;
    EP_HOST_ASSERT((value.empty() or value == "auto") and
                   "EP_HYBRID_KERNEL must be `auto`, `ordered` or `unordered`");
    const auto nic_type = detect_rdma_nic_type(root);
    if (nic_type == RdmaNicType::Unknown)
        printf("[WARN] DeepEP could not identify all RDMA NIC drivers; using unordered hybrid kernels. "
               "Set EP_HYBRID_KERNEL explicitly to override.\n");
    return nic_type == RdmaNicType::NonEfa;
}

} // namespace detail

// Share the first selection across translation units, Python QP allocation and JIT.
// Any visible EFA NIC selects unordered; EP_NIC_NAME only controls rate probing.
inline bool use_ordered_hybrid_kernel() {
    static const bool ordered = [] {
        const auto value = std::getenv("EP_HYBRID_KERNEL");
        const bool result = detail::resolve_ordered_hybrid_kernel(value ? value : "", "/sys/class/infiniband");
        printf("DeepEP hybrid kernel selection: %s\n", result ? "ordered" : "unordered");
        return result;
    }();
    return ordered;
}

} // namespace deep_ep::elastic
