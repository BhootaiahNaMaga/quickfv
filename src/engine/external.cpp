#include "engine/external.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace qfv {

ProcessResult runProcess(const std::vector<std::string>& argv,
                         std::chrono::steady_clock::time_point deadline,
                         const std::atomic<bool>& cancel) {
    ProcessResult r;
    // The system temp dir ($TMPDIR), not a hard-coded /tmp: sandboxed or
    // locked-down machines often forbid writing /tmp.
    std::string path = (std::filesystem::temp_directory_path() / "qfv_proc_XXXXXX").string();
    std::vector<char> tmpl(path.begin(), path.end());
    tmpl.push_back('\0');
    int fd = mkstemp(tmpl.data());
    if (fd < 0) {
        r.output = "qfv: cannot create a temporary file in " + path;
        return r;
    }
    // posix_spawn, not fork+exec: after fork() in a multithreaded process the
    // child may only call async-signal-safe functions (no allocation), and the
    // portfolio runs provers from several threads.
    std::vector<char*> args;
    for (auto& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fd, 1);
    posix_spawn_file_actions_adddup2(&actions, fd, 2);
    // Own process group, so a timeout kills the whole tree: tools are often
    // wrapper scripts (OSS CAD Suite's bin/rIC3 execs libexec/rIC3), and killing
    // only the wrapper leaves the real prover running forever.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    int err = posix_spawnp(&pid, args[0], &actions, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(fd);
    if (err != 0) {
        r.output = "qfv: cannot start " + argv[0];
        std::remove(tmpl.data());
        return r;
    }
    int status = 0;
    while (true) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            r.finished = true;
            r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            kill(-pid, SIGKILL); // the whole process group
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::ifstream in(tmpl.data());
    std::stringstream ss;
    ss << in.rdbuf();
    r.output = ss.str();
    std::remove(tmpl.data());
    return r;
}

bool writeSingleBadBtor2(const std::string& in, const std::string& out, int64_t badId) {
    std::ifstream src(in);
    std::ofstream dst(out);
    if (!src || !dst)
        return false;
    std::string line;
    while (std::getline(src, line)) {
        std::istringstream ls(line);
        int64_t id;
        std::string tag;
        if (ls >> id >> tag && tag == "bad" && id != badId)
            continue;
        dst << line << "\n";
    }
    return true;
}

} // namespace qfv
