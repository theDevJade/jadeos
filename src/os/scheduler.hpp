#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace os {

enum class SigNum : int {
    HUP  =  1,
    INT  =  2,
    QUIT =  3,
    KILL =  9,
    TERM = 15,
    CHLD = 17,
    CONT = 18,
    STOP = 19,
};

// Process states (mirrors /proc/<pid>/status State field)
enum class ProcessState { Ready, Running, Blocked, Terminated };

// A lightweight "process" is just a callable task + metadata.
using TaskFn = std::function<void()>;

struct Process {
    uint32_t     pid              = 0;
    uint32_t     ppid             = 0;
    std::string  name;
    std::string  cmd;
    uint32_t     mem_kb           = 0;
    uint64_t     cpu_ticks        = 0;
    uint64_t     start_tick       = 0;
    ProcessState state            = ProcessState::Ready;
    // Scheduling
    int8_t       nice             = 0;    // -20..+19 (lower = higher priority)
    uint32_t     quantum_ticks    = 4;    // ticks remaining in current time slice
    // IPC
    uint32_t     pending_signals  = 0;    // bitfield: signal N = bit N
    uint32_t     signal_mask      = 0;    // blocked signals bitfield
    uint64_t     sleep_until_tick = 0;    // when Blocked: wake after this tick
    TaskFn       task;
};

// Snapshot for ps/top output (no task fn, safe to copy).
struct PsEntry {
    uint32_t     pid;
    uint32_t     ppid;
    std::string  name;
    std::string  cmd;
    uint32_t     mem_kb;
    uint64_t     cpu_ticks;
    ProcessState state;
    int8_t       nice;
    uint32_t     pending_signals;
};

// Preemptive-style round-robin scheduler with priority, quanta, and signals.
// Conceptually: SCHED_NORMAL (CFS-style nice levels), cooperative task fns.
class Scheduler {
public:
    // Spawn with full metadata (explicit pid lets us set up process tree).
    uint32_t spawn(std::string name, std::string cmd, uint32_t ppid,
                   uint32_t pid, uint32_t mem_kb, TaskFn task);
    // Backward-compat overload.
    uint32_t spawn(std::string name, TaskFn task);

    void     tick();         // Run one process for one timeslice.
    bool     any_alive() const;
    std::size_t process_count() const { return queue_.size() + sleep_queue_.size(); }

    uint32_t current_pid() const noexcept { return current_pid_; }

    // Snapshot of all processes for task manager / ps / top.
    std::vector<PsEntry> all_processes() const;

    // Block pid (move to sleep queue).  If sleep_ticks > 0, auto-wake after
    // that many scheduler ticks (simulates nanosleep / schedule_timeout).
    void block(uint32_t pid, uint64_t sleep_ticks = 0) noexcept;

    // Unblock pid (move back to run queue immediately).
    void unblock(uint32_t pid) noexcept;

    // Deliver signal sig to pid.  SIGKILL/SIGTERM cause termination on next tick.
    void send_signal(uint32_t pid, int sig) noexcept;

    // Return a snapshot of all live processes.
    std::vector<PsEntry> ps() const;

    // Terminate a process by pid (no-op if not found).
    void kill(uint32_t pid);

private:
    std::deque<Process>  queue_;
    std::vector<Process> sleep_queue_;
    uint32_t             next_pid_   = 10; // start above reserved pids
    uint32_t             current_pid_= 0;
    uint64_t             tick_       = 0;

    // Compute time-slice quantum from nice value.
    // nice -20 -> 8 ticks, nice 0 -> 4 ticks, nice +19 -> 1 tick.
    static uint32_t quantum_for(int8_t nice) noexcept;
};

}  // namespace os
