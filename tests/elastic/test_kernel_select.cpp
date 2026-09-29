// CPU-only regression tests; run with test_kernel_select.py.

#include <iostream>
#include <stdexcept>
#include <unistd.h>

#include "../../csrc/kernels/elastic/kernel_select.hpp"

namespace fs = std::filesystem;
using deep_ep::elastic::detail::resolve_ordered_hybrid_kernel;
using deep_ep::elastic::detail::detect_rdma_nic_type;
using deep_ep::elastic::detail::RdmaNicType;

bool selection_from_peer();

static void check(bool condition, const char* message) {
    if (not condition)
        throw std::runtime_error(message);
}

struct Fixture {
    fs::path root;

    Fixture() {
        char pattern[] = "/tmp/deepep-kernel-select-XXXXXX";
        const auto path = mkdtemp(pattern);
        if (not path)
            throw std::runtime_error("mkdtemp failed");
        root = path;
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void nic(const char* name, const char* driver, bool uevent = false) {
        const auto device = root / name / "device";
        fs::create_directories(device);
        if (uevent)
            std::ofstream(device / "uevent") << "PCI_CLASS=20000\nDRIVER=" << driver << "\n";
        else
            fs::create_symlink(fs::path("/sys/bus/pci/drivers") / driver, device / "driver");
    }
};

int main() {
    try {
        Fixture empty;
        check(not resolve_ordered_hybrid_kernel("", empty.root), "empty sysfs must be safe");
        check(not resolve_ordered_hybrid_kernel("auto", empty.root / "missing"), "missing sysfs must be safe");
        std::ofstream(empty.root / "file") << "not a directory";
        check(not resolve_ordered_hybrid_kernel("auto", empty.root / "file"), "invalid sysfs root must be safe");

        Fixture ib;
        ib.nic("mlx5_0", "mlx5_core");
        ib.nic("mlx5_1", "mlx5_core", true);
        check(resolve_ordered_hybrid_kernel("", ib.root), "unset must select ordered on non-EFA");
        check(resolve_ordered_hybrid_kernel("auto", ib.root), "auto must select ordered on non-EFA");
        check(not resolve_ordered_hybrid_kernel("unordered", ib.root), "unordered override must win");

        Fixture efa;
        efa.nic("arbitrary_name", "efa");
        check(detect_rdma_nic_type(efa.root) == RdmaNicType::Efa, "must identify EFA, not fall back to unknown");
        check(not resolve_ordered_hybrid_kernel("", efa.root), "EFA driver must select unordered");
        check(resolve_ordered_hybrid_kernel("ordered", efa.root), "ordered override must win on EFA");
        check(resolve_ordered_hybrid_kernel("ordered", empty.root / "missing"), "override must bypass probing");
        check(not resolve_ordered_hybrid_kernel("unordered", empty.root / "missing"), "override must bypass probing");

        Fixture efa_uevent;
        efa_uevent.nic("rdmap0s1", "efa", true);
        check(detect_rdma_nic_type(efa_uevent.root) == RdmaNicType::Efa, "must identify EFA through uevent");
        check(not resolve_ordered_hybrid_kernel("auto", efa_uevent.root), "uevent EFA detection failed");
        ib.nic("rdmap0s1", "efa");
        check(detect_rdma_nic_type(ib.root) == RdmaNicType::Efa, "EFA must take precedence on mixed hosts");
        check(not resolve_ordered_hybrid_kernel("auto", ib.root), "mixed hosts must select unordered");

        Fixture renamed;
        renamed.nic("rdmap0s1", "mlx5_core");
        check(resolve_ordered_hybrid_kernel("auto", renamed.root), "device name must not override driver identity");

        Fixture unknown;
        fs::create_directories(unknown.root / "rdmap0s1" / "device");
        unknown.nic("mlx5_0", "mlx5_core");
        check(not resolve_ordered_hybrid_kernel("auto", unknown.root), "unidentified NIC must be safe");
        std::ofstream(unknown.root / "rdmap0s1" / "device" / "uevent") << "DRIVER=\n";
        check(not resolve_ordered_hybrid_kernel("auto", unknown.root), "empty driver must be safe");
        fs::permissions(unknown.root, fs::perms::none);
        check(not resolve_ordered_hybrid_kernel("auto", unknown.root), "unreadable sysfs must be safe");
        fs::permissions(unknown.root, fs::perms::owner_all);

        for (const auto value : {"invalid", "ORDERED", " auto", "unordered "}) {
            bool threw = false;
            try {
                resolve_ordered_hybrid_kernel(value, efa.root);
            } catch (const EPException&) {
                threw = true;
            }
            check(threw, "invalid selection must throw");
        }

        setenv("EP_HYBRID_KERNEL", "ordered", 1);
        check(deep_ep::elastic::use_ordered_hybrid_kernel(), "explicit ordered selection failed");
        setenv("EP_HYBRID_KERNEL", "unordered", 1);
        check(deep_ep::elastic::use_ordered_hybrid_kernel(), "selection must stay cached");
        check(selection_from_peer(), "selection must be shared across translation units");
        std::cout << "kernel selection tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
