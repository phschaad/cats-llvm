#ifndef __CATS_RUNTIME_H__
#define __CATS_RUNTIME_H__

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

struct CATS_Event {
    CATS_Event_Type event_type;
    const char *funcname;
    const char *filename;
    int line;
    int col;
};

struct Memory_Event : public CATS_Event {
    const char *buffer_name;
};

struct Allocation_Event : public Memory_Event {
    size_t size;
};

struct Deallocation_Event : public Memory_Event {
};

struct Access_Event : public Memory_Event {
    bool is_write;
};

struct Control_Scope_Event : public CATS_Event {
    int id;
};

enum Scope_Type {
    SCOPE_TYPE_FUNCTION,
    SCOPE_TYPE_LOOP,
    SCOPE_TYPE_CONDITIONAL,
    SCOPE_TYPE_PARALLEL,
    SCOPE_TYPE_UNSTRUCTURED,
};

struct Scope_Entry_Event : public Control_Scope_Event {
    Scope_Type type;
};

struct Scope_Exit_Event : public Control_Scope_Event {
};

class CATS_Trace {
protected:
    std::mutex _mutex;

    std::deque<int> _scope_stack;
    std::map<const void *, std::pair<std::string, size_t>> _allocations;
    std::map<int, std::vector<std::string>> _recorded_calls;
    std::vector<CATS_Event> _events;

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

    bool already_recorded(int call_id) {
        auto it = this->_recorded_calls.find(call_id);
        auto stack_id = this->get_stack_identifier();
        if (it == this->_recorded_calls.end()) {
            this->_recorded_calls[call_id] = std::vector<std::string>();
            this->_recorded_calls[call_id].push_back(stack_id);
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
        return false;
    }

public:
    CATS_Trace() {}

    void reset() {
        std::lock_guard<std::mutex> guard(this->_mutex);
        this->_events.clear();
        this->_events.reserve(CATS_TRACE_BUFFER_SIZE);
        this->_allocations.clear();
        this->_scope_stack.clear();
        this->_recorded_calls.clear();
    }

    void instrument_alloc(
        int call_id,
        const char *buffer_name, void *address, size_t size,
        const char *funcname, const char *filename, int line, int col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        if (!buffer_name || !*buffer_name) {
            buffer_name = "$UNKNOWN$";
        }

        Allocation_Event event;
        event.event_type = EVENT_TYPE_ALLOCATION;
        event.buffer_name = buffer_name;
        event.size = size;
        event.funcname = funcname;
        event.filename = filename;
        event.line = line;
        event.col = col;
        this->_events.push_back(event);

        this->_allocations[address] = std::make_pair(buffer_name, size);
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
            Deallocation_Event event;
            event.event_type = EVENT_TYPE_DEALLOCATION;
            event.buffer_name = it->second.first.c_str();
            event.funcname = funcname;
            event.filename = filename;
            event.line = line;
            event.col = col;
            this->_events.push_back(event);
            this->_allocations.erase(it);
        }
    }

    void instrument_access(
        int call_id,
        void *address, bool is_write, const char *funcname,
        const char *filename, int line, int col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        const char *buffer_name = nullptr;
        auto it = this->_allocations.lower_bound(address);
        if (it->first == address) {
            buffer_name = it->second.first.c_str();
        } else if (it != this->_allocations.begin()) {
            --it;
            if (address < it->first + it->second.second) {
                buffer_name = it->second.first.c_str();
            }
        }

        if (buffer_name != nullptr) {
            Access_Event event;
            event.event_type = EVENT_TYPE_ACCESS;
            event.is_write = is_write;
            event.buffer_name = buffer_name;
            event.funcname = funcname;
            event.filename = filename;
            event.line = line;
            event.col = col;
            this->_events.push_back(event);
        }
    }

    void instrument_read(
        int call_id,
        void *address, const char *funcname, const char *filename,
        int line, int col
    ) {
        this->instrument_access(
            call_id, address, false, funcname, filename, line, col
        );
    }

    void instrument_write(
        int call_id,
        void *address, const char *funcname, const char *filename,
        int line, int col
    ) {
        this->instrument_access(
            call_id, address, true, funcname, filename, line, col
        );
    }

    void instrument_scope_entry(
        int call_id,
        int scope_id, Scope_Type type, const char *funcname,
        const char *filename, int line, int col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        Scope_Entry_Event event;
        event.event_type = EVENT_TYPE_SCOPE_ENTRY;
        event.id = scope_id;
        event.type = type;
        event.funcname = funcname;
        event.filename = filename;
        event.line = line;
        event.col = col;
        this->_events.push_back(event);

        this->_scope_stack.push_back(scope_id);
    }

    void instrument_scope_exit(
        int call_id,
        int scope_id, const char *funcname, const char *filename,
        int line, int col
    ) {
        std::lock_guard<std::mutex> guard(this->_mutex);

        if (this->already_recorded(call_id)) {
            return;
        }

        Scope_Exit_Event event;
        event.event_type = EVENT_TYPE_SCOPE_EXIT;
        event.id = scope_id;
        event.funcname = funcname;
        event.filename = filename;
        event.line = line;
        event.col = col;
        this->_events.push_back(event);

        while (!this->_scope_stack.empty() &&
                this->_scope_stack.back() != scope_id) {
            auto top = this->_scope_stack.back();

            Scope_Exit_Event inferred_event;
            inferred_event.event_type = EVENT_TYPE_SCOPE_EXIT;
            inferred_event.id = top;
            inferred_event.funcname = funcname;
            inferred_event.filename = filename;
            inferred_event.line = line;
            inferred_event.col = col;
            this->_events.push_back(inferred_event);

            this->_scope_stack.pop_back();
        }

        if (this->_scope_stack.empty() || this->_scope_stack.back() != scope_id) {
            printf("Warning: Exiting scope %d not found, this is likely a catastrophic error and will cause an incorrect trace\n", scope_id);
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
            for (size_t i = 0; i < this->_events.size(); ++i) {
                const CATS_Event &event = this->_events[i];
                if (i > 0) {
                    ofs << "," << std::endl;
                }
                ofs << "    {";
                ofs << "\"funcname\": \"" << event.funcname << "\", ";
                ofs << "\"filename\": \"" << event.filename << "\", ";
                ofs << "\"line\": " << event.line << ", ";
                ofs << "\"col\": " << event.col;
                switch (event.event_type) {
                    case EVENT_TYPE_ALLOCATION: {
                        const Allocation_Event &alloc_event =
                            static_cast<const Allocation_Event &>(event);
                        ofs << ", \"type\": \"allocation\", ";
                        ofs << "\"buffer_name\": \"" << alloc_event.buffer_name << "\", ";
                        ofs << "\"size\": " << alloc_event.size;
                        break;
                    }
                    case EVENT_TYPE_DEALLOCATION: {
                        const Deallocation_Event &dealloc_event =
                            static_cast<const Deallocation_Event &>(event);
                        ofs << ", \"type\": \"deallocation\", ";
                        ofs << "\"buffer_name\": \"" << dealloc_event.buffer_name << "\"";
                        break;
                    }
                    case EVENT_TYPE_ACCESS: {
                        const Access_Event &access_event =
                            static_cast<const Access_Event &>(event);
                        ofs << ", \"type\": \"access\", ";
                        ofs << "\"mode\": " << (access_event.is_write ? "w" : "r") << ", ";
                        ofs << "\"buffer_name\": \"" << access_event.buffer_name << "\"";
                        break;
                    }
                    case EVENT_TYPE_SCOPE_ENTRY: {
                        const Scope_Entry_Event &scope_entry_event =
                            static_cast<const Scope_Entry_Event &>(event);
                        ofs << ", \"type\": \"scope_entry\", ";
                        ofs << "\"scope_type\": " << scope_entry_event.type << ", ";
                        ofs << "\"id\": " << scope_entry_event.id;
                        break;
                    }
                    case EVENT_TYPE_SCOPE_EXIT: {
                        const Scope_Exit_Event &scope_exit_event =
                            static_cast<const Scope_Exit_Event &>(event);
                        ofs << ", \"type\": \"scope_exit\", ";
                        ofs << "\"id\": " << scope_exit_event.id;
                        break;
                    }
                }
                ofs << "}" << std::endl;
            }

            ofs << "  ]" << std::endl;
            ofs << "}" << std::endl;
        }
    }

};

} // namespace cats

#undef CATS_TRACE_BUFFER_SIZE

#endif // __CATS_RUNTIME_H__
