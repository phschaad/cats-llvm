// Copyright (c) ETH Zurich, the cats-llvm authors, and Lawrence Livermore National Security, LLC. All rights reserved.

#include "cats_runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_set>
#include <mutex>
#include <sstream>
#include <vector>

#include <omp.h>

#define CATS_STACK_IDENTIFIER_STRATEGY_DEFAULT      0
#define CATS_STACK_IDENTIFIER_STRATEGY_FAST         1
#define CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST    2

#ifndef CATS_STACK_IDENTIFIER_STRATEGY
#define CATS_STACK_IDENTIFIER_STRATEGY CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
#endif

#ifndef CATS_RUNTIME_DEBUG
#define CATS_RUNTIME_DEBUG                          0
#endif
#ifndef CATS_RUNTIME_PRINT_ALLOCATIONS
#define CATS_RUNTIME_PRINT_ALLOCATIONS              0
#endif
#ifndef CATS_RUNTIME_PRINT_ACCESSES
#define CATS_RUNTIME_PRINT_ACCESSES                 0
#endif
#ifndef CATS_RUNTIME_PRINT_SCOPES
#define CATS_RUNTIME_PRINT_SCOPES                   0
#endif

#ifndef CATS_RUNTIME_WARN_ON_SCOPE_EXIT_NOT_FOUND
#define CATS_RUNTIME_WARN_ON_SCOPE_EXIT_NOT_FOUND   0
#endif

#ifndef CATS_TRACE_FILE_NAME_SIZE
#define CATS_TRACE_FILE_NAME_SIZE                   256
#endif
#ifndef CATS_TRACE_FUNC_NAME_SIZE
#define CATS_TRACE_FUNC_NAME_SIZE                   64
#endif
#ifndef CATS_TRACE_BUFFER_NAME_SIZE
#define CATS_TRACE_BUFFER_NAME_SIZE                 64
#endif

namespace cats {

struct CATS_Debug_Info {
  char funcname[CATS_TRACE_FUNC_NAME_SIZE];
  char filename[CATS_TRACE_FILE_NAME_SIZE];
  uint32_t line;
  uint32_t col;
};

struct CATS_Event {
#if CATS_RUNTIME_DEBUG
  uint64_t call_id;
#endif
  uint8_t event_type;
  CATS_Debug_Info debug_info;
  const void *args;
};

struct Allocation_Event_Args {
  char buffer_name[CATS_TRACE_BUFFER_NAME_SIZE];
  uint64_t buffer_id;
  size_t size;
};

struct Deallocation_Event_Args {
  char buffer_name[CATS_TRACE_BUFFER_NAME_SIZE];
  uint64_t buffer_id;
};

struct Access_Event_Args {
  char buffer_name[CATS_TRACE_BUFFER_NAME_SIZE];
  uint64_t buffer_id;
  bool is_write;
};

struct Scope_Entry_Event_Args {
  uint64_t scope_id;
  uint8_t type;
};

struct Scope_Exit_Event_Args {
  uint64_t scope_id;
};

struct CATS_Alloc_Info {
  char buffer_name[CATS_TRACE_BUFFER_NAME_SIZE];
  uint64_t buffer_id;
  size_t size;
};

class CATS_Trace {
protected:
    uint64_t n_events = 0;

    std::mutex _mutex;

    std::deque<uint64_t> _scope_stack;
    std::unordered_set<uint64_t> _scope_ids;
    std::map<const void *, CATS_Alloc_Info> _allocations;
#if CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
    std::map<uint64_t, std::unordered_set<uint64_t>> _recorded_calls;
    uint64_t stack_id = 0;
#elif CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_FAST
    std::map<uint64_t, std::unordered_set<uint64_t>> _recorded_calls;
#else
    std::map<uint64_t, std::vector<std::string>> _recorded_calls;
#endif
    std::deque<CATS_Event *> _events;

    std::string get_stack_identifier() {
      std::stringstream ss;
      auto it = this->_scope_stack.begin();
      while (it != this->_scope_stack.end()) {
        ss << *it;
        ++it;
        if (it != this->_scope_stack.end()) {
          ss << ",";
        }
      }
      return ss.str();
    }

    uint64_t get_stack_identifier_fast() {
      uint64_t identifier = 0;
      bool even = true;
      auto it = this->_scope_stack.begin();
      while (it != this->_scope_stack.end()) {
        if (even)
          identifier += *it;
        else
          identifier -= *it;
        even = !even;
        ++it;
      }
      return identifier;
    }

    bool already_recorded(uint64_t call_id) {
      auto it = this->_recorded_calls.find(call_id);
#if CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_FAST
      uint64_t stack_id = this->get_stack_identifier_fast();
#elif CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
      uint64_t stack_id = this->stack_id;
#else
      auto stack_id = this->get_stack_identifier();
#endif
      if (it == this->_recorded_calls.end()) {
#if CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_FAST || CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
        this->_recorded_calls[call_id] = std::unordered_set<uint64_t>();
        this->_recorded_calls[call_id].insert(stack_id);
#else
        this->_recorded_calls[call_id] = std::vector<std::string>();
        this->_recorded_calls[call_id].push_back(stack_id);
#endif
        return false;
      } else {
#if CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_FAST || CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
        auto sid_id = it->second.find(stack_id);
        if (sid_id != it->second.end())
          return true;
        it->second.insert(stack_id);
#else
        auto val = it->second;
        for (auto &v : val) {
          if (v == stack_id) {
            return true;
          }
        }
        it->second.push_back(stack_id);
#endif
      }
      return false;
    }

    void record_event(uint64_t call_id, uint32_t event_type, const void *args,
                      const char *funcname, const char *filename,
                      uint32_t line, uint32_t col) {
      CATS_Event *event = (CATS_Event *) malloc(sizeof(CATS_Event));
#if CATS_RUNTIME_DEBUG
      event->call_id = call_id;
#endif
      event->event_type = event_type;
      event->args = args;

      if (!funcname || !*funcname)
        funcname = "$UNKNOWN$";
      strncpy(
        event->debug_info.funcname, funcname, CATS_TRACE_FUNC_NAME_SIZE - 1
      );
      event->debug_info.funcname[CATS_TRACE_FUNC_NAME_SIZE - 1] = '\0';

      if (!filename || !*filename)
        filename = "$UNKNOWN$";
      strncpy(
        event->debug_info.filename, filename, CATS_TRACE_FILE_NAME_SIZE - 1
      );
      event->debug_info.filename[CATS_TRACE_FILE_NAME_SIZE - 1] = '\0';

      event->debug_info.line = line;
      event->debug_info.col = col;
      this->_events.push_back(event);

#if CATS_RUNTIME_DEBUG
      if (this->_events.size() % 1'000'000 == 0) {
        std::cout << "Recorded " << this->_events.size() << " events"
                  << std::endl;
      }
#endif
    }

public:
  CATS_Trace() {}

  ~CATS_Trace() {
    this->reset();
  }

  void reset() {
    std::lock_guard<std::mutex> guard(this->_mutex);
    for (auto &event : this->_events) {
      free((void *) event->args);
      free(event);
    }
    this->_events.clear();
    this->_allocations.clear();
    this->_scope_ids.clear();
    this->_scope_stack.clear();
    this->_recorded_calls.clear();
  }

  void instrument_alloc(
    uint64_t call_id,
    const char *buffer_name, void *address, size_t size,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
  ) {
    if (omp_in_parallel() && omp_get_thread_num() != 0) {
      // If we are in a parallel region, only the master thread should exit
      // the scope, so we skip this call.
      return;
    }

    std::lock_guard<std::mutex> guard(this->_mutex);

    if (this->already_recorded(call_id)) {
      // If this call has already been recorded, skip the allocation
      return;
    }

    Allocation_Event_Args *args = (Allocation_Event_Args *) malloc(
      sizeof(Allocation_Event_Args)
    );

    if (!buffer_name || !*buffer_name)
      buffer_name = "$UNKNOWN$";

#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_ALLOCATIONS
    std::cout << "Allocating " << buffer_name <<
              << " at " << address << " in " << funcname << " (" << size
              << " bytes)" << std::endl;
#endif

    strncpy(
      args->buffer_name, buffer_name, CATS_TRACE_BUFFER_NAME_SIZE - 1
    );
    args->buffer_name[CATS_TRACE_BUFFER_NAME_SIZE - 1] = '\0';

    args->size = size;
    args->buffer_id = (size_t) address;
    this->record_event(
      call_id, CATS_EVENT_TYPE_ALLOCATION, args, funcname, filename, line, col
    );

    CATS_Alloc_Info alloc_info;
    strncpy(
      alloc_info.buffer_name, buffer_name, CATS_TRACE_BUFFER_NAME_SIZE - 1
    );
    alloc_info.buffer_name[CATS_TRACE_BUFFER_NAME_SIZE - 1] = '\0';
    alloc_info.buffer_id = (size_t) address;
    alloc_info.size = size;
    this->_allocations[address] = alloc_info;
  }

  void instrument_dealloc(
    uint64_t call_id,
    void *address, const char *funcname, const char *filename,
    uint32_t line, uint32_t col
  ) {
    if (omp_in_parallel() && omp_get_thread_num() != 0) {
      // If we are in a parallel region, only the master thread should exit
      // the scope, so we skip this call.
      return;
    }

    std::lock_guard<std::mutex> guard(this->_mutex);

    if (this->already_recorded(call_id)) {
      // If this call has already been recorded, skip the allocation
      return;
    }

#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_ALLOCATIONS
    std::cout << "Deallocating at " << address
              << " in " << funcname << std::endl;
#endif

    auto it = this->_allocations.find(address);
    if (it != this->_allocations.end()) {
      Deallocation_Event_Args *args = (Deallocation_Event_Args *) malloc(
        sizeof(Deallocation_Event_Args)
      );
      strncpy(
        args->buffer_name, it->second.buffer_name,
        CATS_TRACE_BUFFER_NAME_SIZE - 1
      );
      args->buffer_id = it->second.buffer_id;
      args->buffer_name[CATS_TRACE_BUFFER_NAME_SIZE - 1] = '\0';

#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_ALLOCATIONS
      std::cout << "Deallocating " << it->second.buffer_name
                << std::endl;
#endif

      this->record_event(
        call_id, CATS_EVENT_TYPE_DEALLOCATION, args, funcname, filename, line,
        col
      );
      this->_allocations.erase(it);
    }
  }

  void instrument_access(
    uint64_t call_id,
    void *address, bool is_write, const char *funcname,
    const char *filename, uint32_t line, uint32_t col
  ) {
    if (omp_in_parallel() && omp_get_thread_num() != 0) {
      // If we are in a parallel region, only the master thread should exit
      // the scope, so we skip this call.
      return;
    }

    std::lock_guard<std::mutex> guard(this->_mutex);

    if (this->already_recorded(call_id)) {
      // If this call has already been recorded, skip the allocation
      return;
    }

#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_ACCESSES
    std::cout << "Accessing " << (is_write ? "write" : "read")
              << " at " << address << " in " << funcname
              << std::endl;
#endif

    char *buffer_name = nullptr;
    uint64_t buffer_id = 0;
    auto it = this->_allocations.lower_bound(address);
    if (it != this->_allocations.end() && it->first == address) {
      buffer_name = it->second.buffer_name;
      buffer_id = it->second.buffer_id;
    } else if (it != this->_allocations.begin()) {
      --it;
      if (address <= ((char *) it->first) + it->second.size) {
        buffer_name = it->second.buffer_name;
        buffer_id = it->second.buffer_id;
      }
    }

    const char *actual_buffer_name = buffer_name;
    if (!buffer_name || !*buffer_name)
      actual_buffer_name = "$UNKNOWN$";

    if (buffer_id != 0) {
#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_ACCESSES
      std::cout << "Accessing " << actual_buffer_name << std::endl;
#endif

      Access_Event_Args *args = (Access_Event_Args *) malloc(
        sizeof(Access_Event_Args)
      );
      strncpy(
        args->buffer_name, actual_buffer_name, CATS_TRACE_BUFFER_NAME_SIZE - 1
      );
      args->buffer_name[CATS_TRACE_BUFFER_NAME_SIZE - 1] = '\0';
      args->buffer_id = buffer_id;
      args->is_write = is_write;

      this->record_event(
        call_id, CATS_EVENT_TYPE_ACCESS, args, funcname, filename, line, col
      );
    }
  }

  void instrument_read(
    uint64_t call_id,
    void *address, const char *funcname, const char *filename,
    uint32_t line, uint32_t col
  ) {
    this->instrument_access(
      call_id, address, false, funcname, filename, line, col
    );
  }

  void instrument_write(
    uint64_t call_id,
    void *address, const char *funcname, const char *filename,
    uint32_t line, uint32_t col
  ) {
    this->instrument_access(
      call_id, address, true, funcname, filename, line, col
    );
  }

  void instrument_scope_entry(
    uint64_t call_id,
    uint64_t scope_id, uint8_t type, const char *funcname,
    const char *filename, uint32_t line, uint32_t col
  ) {
    if (omp_in_parallel() && omp_get_thread_num() != 0) {
      // If we are in a parallel region, only the master thread should exit
      // the scope, so we skip this call.
      return;
    }

    std::lock_guard<std::mutex> guard(this->_mutex);

#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_SCOPES
    std::cout << "Entering scope " << scope_id
              << " of type " << (int) type
              << " in " << funcname << std::endl;
#endif

    // The scope must be entered regardless of whether it has been
    // recorded before, so we push it onto the stack
    this->_scope_stack.push_back(scope_id);
    this->_scope_ids.insert(scope_id);

#if CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
    this->stack_id = 0;
    for (auto &id : this->_scope_stack)
      this->stack_id += id;
#endif

    if (this->already_recorded(call_id)) {
      // If this call has already been recorded, skip the allocation
      return;
    }

    Scope_Entry_Event_Args *args = (Scope_Entry_Event_Args *) malloc(
      sizeof(Scope_Entry_Event_Args)
    );
    args->scope_id = scope_id;
    args->type = type;
    this->record_event(
      call_id, CATS_EVENT_TYPE_SCOPE_ENTRY, args, funcname, filename, line, col
    );
  }

  void instrument_scope_exit(
    uint64_t call_id,
    uint64_t scope_id, uint8_t scope_type, const char *funcname,
    const char *filename, uint32_t line, uint32_t col
  ) {
    if (omp_in_parallel() && omp_get_thread_num() != 0) {
      // If we are in a parallel region, only the master thread should exit
      // the scope, so we skip this call.
      return;
    }

    std::lock_guard<std::mutex> guard(this->_mutex);

#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_SCOPES
    std::cout << "Exiting scope " << scope_id
              << " in " << funcname << std::endl;
#endif

    if (this->_scope_ids.erase(scope_id) == 0) {
#if CATS_RUNTIME_WARN_ON_SCOPE_EXIT_NOT_FOUND
        std::cout << "Warning: Scope " << scope_id;
        std::cout << " not found." << std::endl;
#endif
        return;
    }

    bool recorded = false;
    if (this->already_recorded(call_id)) {
      // If this call has already been recorded, skip the allocation
      recorded = true;
    }

    if (!recorded) {
      Scope_Exit_Event_Args *args = (Scope_Exit_Event_Args *) malloc(
        sizeof(Scope_Exit_Event_Args)
      );
      args->scope_id = scope_id;
      this->record_event(
        call_id, CATS_EVENT_TYPE_SCOPE_EXIT, args, funcname, filename, line, col
      );
    }

    while (!this->_scope_stack.empty() &&
           this->_scope_stack.back() != scope_id) {
      auto top = this->_scope_stack.back();

      if (!recorded) {
        Scope_Exit_Event_Args *inferred = (Scope_Exit_Event_Args *) malloc(
          sizeof(Scope_Exit_Event_Args)
        );
        inferred->scope_id = top;
        this->record_event(
          call_id, CATS_EVENT_TYPE_SCOPE_EXIT, inferred, funcname, filename,
          line, col
        );
      }

#if CATS_RUNTIME_DEBUG && CATS_RUNTIME_PRINT_SCOPES
    std::cout << " -> Exiting scope " << top
              << " as a consequence" << std::endl;
#endif

      this->_scope_stack.pop_back();
      if (this->_scope_ids.erase(top) == 0) {
#if CATS_RUNTIME_WARN_ON_SCOPE_EXIT_NOT_FOUND
        std::cout << "Warning: Scope " << scope_id;
        std::cout << " not found." << std::endl;
#endif
      }
#if CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
      this->stack_id = 0;
      for (auto &id : this->_scope_stack)
        this->stack_id += id;
#endif
    }

    if (this->_scope_stack.empty() || this->_scope_stack.back() != scope_id) {
#if CATS_RUNTIME_WARN_ON_SCOPE_EXIT_NOT_FOUND
      if (scope_type != CATS_SCOPE_TYPE_PARALLEL) {
        // We suppress the warning for parallel scopes since there can be
        // multiple exits from parallel regions.
        // TODO: This is a workaround, we should handle parallel scopes
        // differently and correctly insert only one exit.
        std::cout << "Warning: Exiting scope " << scope_id << " not found. ";
        std::cout << "This is likely an error leading to an incorrect trace. ";
        std::cout << "(Scope type:" << (int) scope_type << ")";
        std::cout << std::endl;
      }
#endif
    } else {
      this->_scope_stack.pop_back();
#if CATS_STACK_IDENTIFIER_STRATEGY == CATS_STACK_IDENTIFIER_STRATEGY_VERY_FAST
      this->stack_id = 0;
      for (auto &id : this->_scope_stack)
        this->stack_id += id;
#endif
    }
  }

  void save(const char *filepath) {
    std::lock_guard<std::mutex> guard(this->_mutex);

    std::stringstream ss;
    ss << "cats_trace.cats";

    {
      std::ofstream ofs(ss.str(), std::ios::binary);

      ofs << "{" << std::endl;
      ofs << "  \"events\": [" << std::endl;
      auto it = this->_events.begin();
      while (it != this->_events.end()) {
        CATS_Event *event = *it;
        if (it != this->_events.begin()) {
          ofs << "," << std::endl;
        }
        ++it;
        ofs << "    {";
#if CATS_RUNTIME_DEBUG
        ofs << "\"call_id\": " << event->call_id << ", ";
#endif
        ofs << "\"funcname\": \"";
        ofs << event->debug_info.funcname << "\", ";
        ofs << "\"filename\": \"";
        ofs << event->debug_info.filename << "\", ";
        ofs << "\"line\": " << event->debug_info.line << ", ";
        ofs << "\"col\": " << event->debug_info.col;
        switch (event->event_type) {
          case CATS_EVENT_TYPE_ALLOCATION: {
            Allocation_Event_Args *args = (Allocation_Event_Args *) event->args;
            ofs << ", \"type\": \"allocation\", ";
            ofs << "\"buffer_name\": \"";
            ofs << args->buffer_name << "\", ";
            ofs << "\"buffer_id\": " << args->buffer_id << ", ";
            ofs << "\"size\": " << args->size;
            break;
          }
          case CATS_EVENT_TYPE_DEALLOCATION: {
            Deallocation_Event_Args *args =
              (Deallocation_Event_Args *) event->args;
            ofs << ", \"type\": \"deallocation\", ";
            ofs << "\"buffer_name\": \"";
            ofs << args->buffer_name << "\", ";
            ofs << "\"buffer_id\": " << args->buffer_id;
            break;
          }
          case CATS_EVENT_TYPE_ACCESS: {
            Access_Event_Args *args = (Access_Event_Args *) event->args;
            ofs << ", \"type\": \"access\", ";
            ofs << "\"mode\": ";
            ofs << (args->is_write ? "\"w\"" : "\"r\"") << ", ";
            ofs << "\"buffer_name\": \"";
            ofs << args->buffer_name << "\", ";
            ofs << "\"buffer_id\": " << args->buffer_id;
            break;
          }
          case CATS_EVENT_TYPE_SCOPE_ENTRY: {
            Scope_Entry_Event_Args*args =
              (Scope_Entry_Event_Args *) event->args;
            ofs << ", \"type\": \"scope_entry\", ";
            switch (args->type) {
              case CATS_SCOPE_TYPE_FUNCTION:
                ofs << "\"scope_type\": \"func\", ";
                break;
              case CATS_SCOPE_TYPE_LOOP:
                ofs << "\"scope_type\": \"loop\", ";
                break;
              case CATS_SCOPE_TYPE_CONDITIONAL:
                ofs << "\"scope_type\": \"cond\", ";
                break;
              case CATS_SCOPE_TYPE_PARALLEL:
                ofs << "\"scope_type\": \"para\", ";
                break;
              case CATS_SCOPE_TYPE_UNSTRUCTURED:
                ofs << "\"scope_type\": \"unst\", ";
                break;
              default:
                ofs << "\"scope_type\": \"n/a\", ";
            }
            ofs << "\"id\": " << args->scope_id;
            break;
          }
          case CATS_EVENT_TYPE_SCOPE_EXIT: {
            Scope_Exit_Event_Args *args = (Scope_Exit_Event_Args *) event->args;
            ofs << ", \"type\": \"scope_exit\", ";
            ofs << "\"id\": " << args->scope_id;
            break;
          }
        }
        ofs << "}";
      }

      ofs << std::endl << "  ]" << std::endl;
      ofs << "}" << std::endl;
    }
  }

};

} // namespace cats

#undef CATS_TRACE_BUFFER_SIZE

extern "C" {

// Global CATS_Trace instance
static cats::CATS_Trace g_cats_trace;

void cats_trace_reset() {
  g_cats_trace.reset();
}

void cats_trace_instrument_alloc(
  uint64_t call_id, const char *buffer_name, void *address, size_t size,
  const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
  g_cats_trace.instrument_alloc(
    call_id, buffer_name, address, size, funcname, filename, line, col
  );
}

void cats_trace_instrument_dealloc(
  uint64_t call_id, void *address,
  const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
  g_cats_trace.instrument_dealloc(
    call_id, address, funcname, filename, line, col
  );
}

void cats_trace_instrument_access(
  uint64_t call_id, void *address, bool is_write,
  const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
  g_cats_trace.instrument_access(
    call_id, address, is_write, funcname, filename, line, col
  );
}

void cats_trace_instrument_read(
  uint64_t call_id, void *address,
  const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
  g_cats_trace.instrument_read(
    call_id, address, funcname, filename, line, col
  );
}

void cats_trace_instrument_write(
  uint64_t call_id, void *address,
  const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
  g_cats_trace.instrument_write(
    call_id, address, funcname, filename, line, col
  );
}

void cats_trace_instrument_scope_entry(
  uint64_t call_id, uint64_t scope_id, uint8_t scope_type,
  const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
  g_cats_trace.instrument_scope_entry(
    call_id, scope_id, scope_type,
    funcname, filename, line, col
  );
}

void cats_trace_instrument_scope_exit(
  uint64_t call_id, uint64_t scope_id, uint8_t scope_type,
  const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
  g_cats_trace.instrument_scope_exit(
    call_id, scope_id, scope_type, funcname, filename, line, col
  );
}

void cats_trace_save(const char *filepath) {
  g_cats_trace.save(filepath);
}

} // extern "C"
