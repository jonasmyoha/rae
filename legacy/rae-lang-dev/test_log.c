#include "rae_runtime.h"
#include <stdio.h>

int main() {
    rae_ext_rae_log_stream_any(rae_any("Hello "));
    rae_ext_rae_log_stream_any(rae_any("World"));
    rae_ext_rae_log_any(rae_any(""));
    rae_ext_rae_log_any(rae_any("Second line"));
    return 0;
}
