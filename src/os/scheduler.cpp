#include "scheduler.hpp"
#include <algorithm>
#include <cstdlib>

namespace os {

uint32_t Scheduler::quantum_for(int8_t nice) noexcept {
    // nice -20..+19 maps to 8..1 ticks (higher priority -> larger slice).
    const int q = (20 - static_cast<int>(nice)) / 5;
    return static_cast<uint32_t>(q < 1 ? 1 : q);
}

uint32_t Scheduler::spawn(std::string name, std::string cmd, uint32_t ppid,
                           uint32_t pid, uint32_t mem_kb, TaskFn task) {
    if (pid == 0 && !name.empty() && name[0] == '[') pid = 0;
    else if (pid == 0) pid = next_pid_++;
    if (pid >= next_pid_) next_pid_ = pid + 1;
    Process p;
    p.pid    = pid;  p.ppid  = ppid;
    p.name   = std::move(name);  p.cmd = std::move(cmd);
    p.mem_kb = mem_kb;  p.start_tick = tick_;
    p.quantum_ticks = quantum_for(p.nice);
    p.state  = ProcessState::Ready;
    p.task   = std::move(task);
    queue_.push_back(std::move(p));
    return pid;
}

uint32_t Scheduler::spawn(std::string name, TaskFn task) {
    return spawn(std::move(name), "", 0, next_pid_, 0, std::move(task));
}

void Scheduler::tick() {
    ++tick_;

    // 1. Wake sleeping processes whose timeout has elapsed.
    for (auto it = sleep_queue_.begin(); it != sleep_queue_.end(); ) {
        if (tick_ >= it->sleep_until_tick) {
            it->state = ProcessState::Ready;
            queue_.push_back(std::move(*it));
            it = sleep_queue_.erase(it);
        } else {
            ++it;
        }
    }

    if (queue_.empty()) return;

    // 2. Select highest-priority (lowest nice) runnable process.
    //    Ties broken by FIFO order (front of deque wins).
    auto best = queue_.begin();
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->nice < best->nice) best = it;
    }

    // 3. Pop and run.
    Process p = std::move(*best);
    queue_.erase(best);

    current_pid_ = p.pid;
    p.state = ProcessState::Running;
    if (p.task) {
        p.task();
        ++p.cpu_ticks;
    }

    // 4. Signal delivery (after task runs, before re-queue).
    const uint32_t deliverable = p.pending_signals & ~p.signal_mask;
    if (deliverable) {
        if (deliverable & (1u << static_cast<int>(SigNum::KILL))) {
            p.state = ProcessState::Terminated;
        } else if (deliverable & (1u << static_cast<int>(SigNum::TERM))) {
            p.state = ProcessState::Terminated;
        }
        p.pending_signals &= ~deliverable;
    }

    if (p.state == ProcessState::Terminated) return;

    // 5. Time-slice accounting.
    p.state = ProcessState::Ready;
    if (p.quantum_ticks > 0) --p.quantum_ticks;
    if (p.quantum_ticks == 0) {
        // Quantum expired: reset and push to back (preempt / round-robin).
        p.quantum_ticks = quantum_for(p.nice);
        queue_.push_back(std::move(p));
    } else {
        // Still has budget: push to front so it runs again next tick
        // before lower-priority processes (simulates non-preemptive slice).
        queue_.push_front(std::move(p));
    }
}

void Scheduler::block(uint32_t pid, uint64_t sleep_ticks) noexcept {
    auto it = std::find_if(queue_.begin(), queue_.end(),
                           [pid](const Process& q){ return q.pid == pid; });
    if (it == queue_.end()) return;
    it->state            = ProcessState::Blocked;
    it->sleep_until_tick = (sleep_ticks > 0) ? (tick_ + sleep_ticks) : UINT64_MAX;
    sleep_queue_.push_back(std::move(*it));
    queue_.erase(it);
}

void Scheduler::unblock(uint32_t pid) noexcept {
    auto it = std::find_if(sleep_queue_.begin(), sleep_queue_.end(),
                           [pid](const Process& q){ return q.pid == pid; });
    if (it == sleep_queue_.end()) return;
    it->state            = ProcessState::Ready;
    it->sleep_until_tick = 0;
    queue_.push_back(std::move(*it));
    sleep_queue_.erase(it);
}

void Scheduler::send_signal(uint32_t pid, int sig) noexcept {
    if (sig < 0 || sig >= 32) return;
    const uint32_t bit = 1u << static_cast<unsigned>(sig);
    // Search run queue.
    for (auto& p : queue_) {
        if (p.pid == pid) { p.pending_signals |= bit; return; }
    }
    // Search sleep queue.
    for (auto& p : sleep_queue_) {
        if (p.pid == pid) { p.pending_signals |= bit; return; }
    }
}

std::vector<PsEntry> Scheduler::all_processes() const {
    std::vector<PsEntry> out;
    auto add = [&](const Process& p) {
        PsEntry e;
        e.pid             = p.pid;
        e.ppid            = p.ppid;
        e.name            = p.name;
        e.cmd             = p.cmd;
        e.mem_kb          = p.mem_kb;
        e.cpu_ticks       = p.cpu_ticks;
        e.state           = p.state;
        e.nice            = p.nice;
        e.pending_signals = p.pending_signals;
        out.push_back(std::move(e));
    };
    for (const auto& p : queue_)       add(p);
    for (const auto& p : sleep_queue_) add(p);
    return out;
}

bool Scheduler::any_alive() const { return !queue_.empty() || !sleep_queue_.empty(); }

std::vector<PsEntry> Scheduler::ps() const {
    std::vector<PsEntry> out;
    out.reserve(queue_.size() + sleep_queue_.size());
    for (const auto& p : queue_)
        out.push_back({ p.pid, p.ppid, p.name, p.cmd,
                        p.mem_kb, p.cpu_ticks, p.state,
                        p.nice,   p.pending_signals });
    for (const auto& p : sleep_queue_)
        out.push_back({ p.pid, p.ppid, p.name, p.cmd,
                        p.mem_kb, p.cpu_ticks, p.state,
                        p.nice,   p.pending_signals });
    std::sort(out.begin(), out.end(),
              [](const PsEntry& a, const PsEntry& b){ return a.pid < b.pid; });
    return out;
}

void Scheduler::kill(uint32_t pid) {
    auto it = std::find_if(queue_.begin(), queue_.end(),
                           [pid](const Process& p){ return p.pid == pid; });
    if (it != queue_.end()) { queue_.erase(it); return; }
    auto jt = std::find_if(sleep_queue_.begin(), sleep_queue_.end(),
                           [pid](const Process& p){ return p.pid == pid; });
    if (jt != sleep_queue_.end()) sleep_queue_.erase(jt);
}

}  // namespace os
