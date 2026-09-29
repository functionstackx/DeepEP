#include "../../csrc/kernels/elastic/kernel_select.hpp"

bool selection_from_peer() {
    return deep_ep::elastic::use_ordered_hybrid_kernel();
}
