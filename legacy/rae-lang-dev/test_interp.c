#include "rae_runtime.h"
#include <stdio.h>

int main() {
    const char* name = "Rae";
    int64_t version = 1;
    rae_ext_rae_log_stream_any(rae_any("Hello ")), rae_ext_rae_log_stream_any(rae_any(name)), rae_ext_rae_log_stream_any(rae_any(", version ")), rae_ext_rae_log_stream_any(rae_any(version)), rae_ext_rae_log_stream_any(rae_any("!")), rae_ext_rae_log_any(rae_any(""));
    rae_ext_rae_log_stream_any(rae_any("Math: ")), rae_ext_rae_log_stream_any(rae_any(1 + 2 * 3)), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_any(rae_any(""));
    return 0;
}
