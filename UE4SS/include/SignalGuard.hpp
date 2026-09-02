#pragma once
#include <functional>
#ifdef __linux__
#include <csignal>
#include <csetjmp>
namespace RC {
    inline thread_local sigjmp_buf* g_signal_jump_buf = nullptr;
    inline thread_local bool g_signal_active = false;
    inline void signal_handler(int sig, siginfo_t*, void*) {
        if (g_signal_active && g_signal_jump_buf) {
            siglongjmp(*g_signal_jump_buf, sig);
        }
    }
    class SignalGuard {
    public:
        static bool safe_call(const std::function<void()>& func) {
            // Use heap allocation for env to avoid stack overflow corruption
            sigjmp_buf* env = (sigjmp_buf*)::operator new(sizeof(sigjmp_buf), std::nothrow);
            if (!env) return false;
            struct sigaction old_segv{}, old_bus{}, old_ill{}, old_fpe{}, old_abrt{};
            struct sigaction new_act{};
            new_act.sa_sigaction = signal_handler;
            new_act.sa_flags = SA_SIGINFO | SA_NODEFER;
            sigemptyset(&new_act.sa_mask);
            sigaction(SIGSEGV, &new_act, &old_segv);
            sigaction(SIGBUS, &new_act, &old_bus);
            sigaction(SIGILL, &new_act, &old_ill);
            sigaction(SIGFPE, &new_act, &old_fpe);
            sigaction(SIGABRT, &new_act, &old_abrt);
            // Save previous global state for nesting
            auto* prev_buf = g_signal_jump_buf;
            bool prev_active = g_signal_active;
            g_signal_jump_buf = env;
            g_signal_active = true;
            bool success = false;
            int sig = sigsetjmp(*env, 1);
            if (sig == 0) {
                try {
                    func();
                    success = true;
                } catch (...) {
                    success = false;
                }
            } else {
                std::fprintf(stderr, "SignalGuard caught signal %d\n", sig);
                std::fflush(stderr);
                success = false;
            }
            g_signal_active = prev_active;
            g_signal_jump_buf = prev_buf;
            sigaction(SIGSEGV, &old_segv, nullptr);
            sigaction(SIGBUS, &old_bus, nullptr);
            sigaction(SIGILL, &old_ill, nullptr);
            sigaction(SIGFPE, &old_fpe, nullptr);
            sigaction(SIGABRT, &old_abrt, nullptr);
            ::operator delete(env);
            return success;
        }
    };
}
#else
namespace RC {
    class SignalGuard {
    public:
        static bool safe_call(const std::function<void()>& func) {
            try { func(); return true; } catch (...) { return false; }
        }
    };
}
#endif
