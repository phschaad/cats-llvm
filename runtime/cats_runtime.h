#ifndef __CATS_RUNTIME_H__
#define __CATS_RUNTIME_H__

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include <stddef.h>
#include <stdint.h>
#endif

#if defined(_WIN32)
  #define CATS_RUNTIME_API __declspec(dllexport)
#else
  #define CATS_RUNTIME_API __attribute__((visibility("default")))
#endif

#define CATS_EVENT_TYPE_ALLOCATION      0
#define CATS_EVENT_TYPE_DEALLOCATION    1
#define CATS_EVENT_TYPE_ACCESS          2
#define CATS_EVENT_TYPE_SCOPE_ENTRY     3
#define CATS_EVENT_TYPE_SCOPE_EXIT      4

#define CATS_SCOPE_TYPE_FUNCTION        0
#define CATS_SCOPE_TYPE_LOOP            1
#define CATS_SCOPE_TYPE_CONDITIONAL     2
#define CATS_SCOPE_TYPE_PARALLEL        3
#define CATS_SCOPE_TYPE_UNSTRUCTURED    4


CATS_RUNTIME_API void cats_trace_reset();

CATS_RUNTIME_API void cats_trace_instrument_alloc(
    uint32_t call_id, const char *buffer_name, void *address, size_t size,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
);

CATS_RUNTIME_API void cats_trace_instrument_dealloc(
    uint32_t call_id, void *address,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
);

CATS_RUNTIME_API void cats_trace_instrument_access(
    uint32_t call_id, void *address, bool is_write,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
);

CATS_RUNTIME_API void cats_trace_instrument_read(
    uint32_t call_id, void *address,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
);

CATS_RUNTIME_API void cats_trace_instrument_write(
    uint32_t call_id, void *address,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
);

CATS_RUNTIME_API void cats_trace_instrument_scope_entry(
    uint32_t call_id, uint32_t scope_id, uint8_t scope_type,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
);

CATS_RUNTIME_API void cats_trace_instrument_scope_exit(
    uint32_t call_id, uint32_t scope_id,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
);

CATS_RUNTIME_API void cats_trace_save(const char *filepath);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // __CATS_RUNTIME_H__
