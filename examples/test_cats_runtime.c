#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "../runtime/cats_runtime.h"

int main() {
    cats_trace_reset();

    // Simulate a function scope
    cats_trace_instrument_scope_entry(1, 100, CATS_SCOPE_TYPE_CONDITIONAL, __func__, __FILE__, __LINE__, 0);

    // Simulate an allocation
    int *arr = (int*)malloc(10 * sizeof(int));
    cats_trace_instrument_alloc(2, "arr", arr, 10 * sizeof(int), __func__, __FILE__, __LINE__, 0);

    // Simulate a write
    arr[0] = 42;
    cats_trace_instrument_write(3, arr, __func__, __FILE__, __LINE__, 0);

    // Simulate a read
    int x = arr[0];
    cats_trace_instrument_read(4, arr, __func__, __FILE__, __LINE__, 0);

    // Simulate deallocation
    cats_trace_instrument_dealloc(5, arr, __func__, __FILE__, __LINE__, 0);
    free(arr);

    // Simulate function scope exit
    cats_trace_instrument_scope_exit(6, 100, __func__, __FILE__, __LINE__, 0);

    // Save the trace
    cats_trace_save("cats_trace_test.json");

    printf("Test trace written to cats_trace_test.json\n");
    return 0;
}
