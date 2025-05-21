#include "cats_runtime.h"

#include <iostream>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#define CATS_TRACE_BUFFER_SIZE 1024

namespace cats {

enum CATS_Event_Type {
    EVENT_TYPE_ALLOCATION,
    EVENT_TYPE_DEALLOCATION,
    EVENT_TYPE_ACCESS,
    EVENT_TYPE_SCOPE_ENTRY,
    EVENT_TYPE_SCOPE_EXIT,
};

struct CATS_Debug_Info {
    std::string funcname;
    std::string filename;
    uint32_t line;
    uint32_t col;
};

struct CATS_Event {
    CATS_Event_Type event_type;
    CATS_Debug_Info debug_info;
    const void *args;
};

struct Allocation_Event_Args {
    std::string buffer_name;
    size_t size;
};

struct Deallocation_Event_Args {
    std::string buffer_name;
};

struct Access_Event_Args {
    std::string buffer_name;
    bool is_write;
};

struct Scope_Entry_Event_Args {
    uint32_t scope_id;
    CATS_SCOPE_TYPE type;
};

struct Scope_Exit_Event_Args {
    uint32_t scope_id;
};

class CATS_Trace {
protected:
    std::mutex _mutex;

    std::deque<int> _scope_stack;
    std::map<const void *, std::pair<std::string, size_t>> _allocations;
    std::map<uint32_t, std::vector<std::string>> _recorded_calls;
    std::deque<CATS_Event> _events;

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

    bool already_recorded(uint32_t call_id) {
        auto it = this->_recorded_calls.find(call_id);
        auto stack_id = this->get_stack_identifier();
        if (it == this->_recorded_calls.end()) {
            this->_recorded_calls[call_id] = std::vector<std::string>();
            this->_recorded_calls[call_id].push_back(stack_id);
            std::cout << "First time recording call_id " << call_id
                      << " with stack id " << stack_id << std::endl;
            std::cout.flush();
            return false;
        } else {
            auto val = it->second;
            for (auto &v : val) {
                if (v == stack_id) {
                    return true;
                }
            }
            it->second.push_back(stack_id);
        }
        std::cout << "First time recording call_id " << call_id
                    << " with stack id " << stack_id << std::endl;
        std::cout.flush();
        return false;
    }

    void record_event(CATS_Event_Type event_type, const void *args,
                      const char *funcname, const char *filename,
                      uint32_t line, uint32_t col) {
        CATS_Event event;
        event.event_type = event_type;
        event.args = args;
        event.debug_info.funcname = funcname ? funcname : "";
        event.debug_info.filename = filename ? filename : "";
        event.debug_info.line = line;
        event.debug_info.col = col;
        this->_events.push_back(event);
    }

public:
    CATS_Trace() {}

    void reset() {
        std::lock_guard<std::mutex> guard(this->_mutex);
        this->_events.clear();
        this->_allocations.clear();
        this->_scope_stack.clear();
        this->_recorded_calls.clear();
    }

    void instrument_alloc(
        uint32_t call_id,
        const char *buffer_name, void *address, size_t size,
        const char *funcname, const char *filename, uint32_t line, uint32_t col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        std::string buffer_name_str = (
            !buffer_name || !*buffer_name
        ) ? "$UNKNOWN$" : buffer_name;

        std::cout << "Allocating " << buffer_name_str
                  << " at " << address << " of size " << size << std::endl;
        std::cout.flush();

        Allocation_Event_Args args;
        args.buffer_name = buffer_name_str;
        args.size = size;
        this->record_event(
            EVENT_TYPE_ALLOCATION, &args,
            funcname, filename, line, col
        );

        this->_allocations[address] = std::make_pair(buffer_name_str, size);
    }

    void instrument_dealloc(
        int call_id,
        void *address, const char *funcname, const char *filename,
        int line, int col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        auto it = this->_allocations.find(address);
        if (it != this->_allocations.end()) {
            Deallocation_Event_Args args;
            args.buffer_name = it->second.first;

            std::cout << "Deallocating " << args.buffer_name
                      << " at " << address << std::endl;
            std::cout.flush();

            this->record_event(
                EVENT_TYPE_DEALLOCATION, &args,
                funcname, filename, line, col
            );
            this->_allocations.erase(it);
        }
    }

    void instrument_access(
        uint32_t call_id,
        void *address, bool is_write, const char *funcname,
        const char *filename, uint32_t line, uint32_t col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        std::string buffer_name;
        auto it = this->_allocations.lower_bound(address);
        if (it != this->_allocations.end() && it->first == address) {
            buffer_name = it->second.first;
        } else if (it != this->_allocations.begin()) {
            --it;
            if (address < ((char *) it->first) + it->second.second) {
                buffer_name = it->second.first;
            }
        }

        if (!buffer_name.empty()) {
            Access_Event_Args args;
            args.buffer_name = buffer_name;
            args.is_write = is_write;

            std::cout << (is_write ? "Writing" : "Reading")
                      << " " << buffer_name
                      << " at " << address << std::endl;
            std::cout.flush();

            this->record_event(
                EVENT_TYPE_ACCESS, &args,
                funcname, filename, line, col
            );
        }
    }

    void instrument_read(
        uint32_t call_id,
        void *address, const char *funcname, const char *filename,
        uint32_t line, uint32_t col
    ) {
        this->instrument_access(
            call_id, address, false, funcname, filename, line, col
        );
    }

    void instrument_write(
        uint32_t call_id,
        void *address, const char *funcname, const char *filename,
        uint32_t line, uint32_t col
    ) {
        this->instrument_access(
            call_id, address, true, funcname, filename, line, col
        );
    }

    void instrument_scope_entry(
        uint32_t call_id,
        uint32_t scope_id, CATS_SCOPE_TYPE type, const char *funcname,
        const char *filename, uint32_t line, uint32_t col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        Scope_Entry_Event_Args args;
        args.scope_id = scope_id;
        args.type = type;
        this->record_event(
            EVENT_TYPE_SCOPE_ENTRY, &args,
            funcname, filename, line, col
        );

        this->_scope_stack.push_back(scope_id);
    }

    void instrument_scope_exit(
        uint32_t call_id,
        uint32_t scope_id, const char *funcname, const char *filename,
        uint32_t line, uint32_t col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        Scope_Exit_Event_Args args;
        args.scope_id = scope_id;
        this->record_event(
            EVENT_TYPE_SCOPE_EXIT, &args,
            funcname, filename, line, col
        );

        while (!this->_scope_stack.empty() &&
                this->_scope_stack.back() != scope_id) {
            auto top = this->_scope_stack.back();

            Scope_Exit_Event_Args inferred;
            inferred.scope_id = top;
            this->record_event(
                EVENT_TYPE_SCOPE_EXIT, &inferred,
                funcname, filename, line, col
            );

            this->_scope_stack.pop_back();
        }

        if (this->_scope_stack.empty() || this->_scope_stack.back() != scope_id) {
            printf("Warning: Exiting scope %d not found.\n", scope_id);
            printf("  This is likely an error leading to an incorrect trace\n");
        } else {
            this->_scope_stack.pop_back();
        }
    }

    void save(const char *filepath) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        std::stringstream ss;
        ss << filepath;

        {
            std::ofstream ofs(ss.str(), std::ios::binary);

            ofs << "{" << std::endl;
            ofs << "  \"events\": [" << std::endl;
            auto it = this->_events.begin();
            while (it != this->_events.end()) {
                CATS_Event event = *it;
                if (it != this->_events.begin()) {
                    ofs << "," << std::endl;
                }
                ++it;
                ofs << "    {";
                ofs << "\"funcname\": \"";
                ofs << event.debug_info.funcname << "\", ";
                ofs << "\"filename\": \"";
                ofs << event.debug_info.filename << "\", ";
                ofs << "\"line\": " << event.debug_info.line << ", ";
                ofs << "\"col\": " << event.debug_info.col;
                switch (event.event_type) {
                    case EVENT_TYPE_ALLOCATION: {
                        Allocation_Event_Args *args =
                            (Allocation_Event_Args *) event.args;
                        ofs << ", \"type\": \"allocation\", ";
                        ofs << "\"buffer_name\": \"";
                        ofs << args->buffer_name << "\", ";
                        ofs << "\"size\": " << args->size;
                        break;
                    }
                    case EVENT_TYPE_DEALLOCATION: {
                        Deallocation_Event_Args *args =
                            (Deallocation_Event_Args *) event.args;
                        ofs << ", \"type\": \"deallocation\", ";
                        ofs << "\"buffer_name\": \"";
                        ofs << args->buffer_name << "\"";
                        break;
                    }
                    case EVENT_TYPE_ACCESS: {
                        Access_Event_Args *args =
                            (Access_Event_Args *) event.args;
                        ofs << ", \"type\": \"access\", ";
                        ofs << "\"mode\": ";
                        ofs << (args->is_write ? "\"w\"" : "\"r\"") << ", ";
                        ofs << "\"buffer_name\": \"";
                        ofs << args->buffer_name << "\"";
                        break;
                    }
                    case EVENT_TYPE_SCOPE_ENTRY: {
                        Scope_Entry_Event_Args*args =
                            (Scope_Entry_Event_Args *) event.args;
                        ofs << ", \"type\": \"scope_entry\", ";
                        ofs << "\"scope_type\": " << args->type << ", ";
                        ofs << "\"id\": " << args->scope_id;
                        break;
                    }
                    case EVENT_TYPE_SCOPE_EXIT: {
                        Scope_Exit_Event_Args *args =
                            (Scope_Exit_Event_Args *) event.args;
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
    uint32_t call_id, const char *buffer_name, void *address, size_t size,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
    g_cats_trace.instrument_alloc(
        call_id, buffer_name, address, size, funcname, filename, line, col
    );
}

void cats_trace_instrument_dealloc(
    uint32_t call_id, void *address,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
    g_cats_trace.instrument_dealloc(
        call_id, address, funcname, filename, line, col
    );
}

void cats_trace_instrument_access(
    uint32_t call_id, void *address, bool is_write,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
    g_cats_trace.instrument_access(
        call_id, address, is_write, funcname, filename, line, col
    );
}

void cats_trace_instrument_read(
    uint32_t call_id, void *address,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
    g_cats_trace.instrument_read(
        call_id, address, funcname, filename, line, col
    );
}

void cats_trace_instrument_write(
    uint32_t call_id, void *address,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
    g_cats_trace.instrument_write(
        call_id, address, funcname, filename, line, col
    );
}

void cats_trace_instrument_scope_entry(
    uint32_t call_id, uint32_t scope_id, CATS_SCOPE_TYPE scope_type,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
    g_cats_trace.instrument_scope_entry(
        call_id, scope_id, scope_type,
        funcname, filename, line, col
    );
}

void cats_trace_instrument_scope_exit(
    uint32_t call_id, uint32_t scope_id,
    const char *funcname, const char *filename, uint32_t line, uint32_t col
) {
    g_cats_trace.instrument_scope_exit(
        call_id, scope_id, funcname, filename, line, col
    );
}

void cats_trace_save(const char *filepath) {
    g_cats_trace.save(filepath);
}

} // extern "C"
