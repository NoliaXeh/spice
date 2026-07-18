#ifndef SPICE_PLUGIN_PLUGINPROCESS_H
#define SPICE_PLUGIN_PLUGINPROCESS_H

#include "spice/plugin/Message.hpp"
#include <string>
#include <sys/types.h>
#include <vector>

namespace spice::plugin {

//! A plugin subprocess speaking the protocol over pipes: the core writes
//! length-prefixed frames to its stdin and reads them from its stdout,
//! both non-blocking so a slow or hung plugin never stalls the core
//! (invariant 1). stderr is captured separately for the log. The process
//! is terminated and reaped on destruction. POSIX only.
class PluginProcess {
public:
    PluginProcess() = default;
    ~PluginProcess();

    PluginProcess(PluginProcess const&) = delete;
    auto operator=(PluginProcess const&) -> PluginProcess& = delete;

    //! Starts `argv`; false on failure.
    auto spawn(std::vector<std::string> const& argv) -> bool;

    //! Still running? Updated by poll() when the child hangs up.
    auto running() const -> bool;

    //! True when the child exited by itself with status 0 - a clean exit,
    //! as opposed to a crash, a signal, or an unobserved death. False
    //! while it is still running.
    auto exited_cleanly() const -> bool;

    //! Queues a frame for the plugin (best-effort, non-blocking write).
    auto send(Message const& message) -> void;

    //! Reads whatever the plugin wrote and returns the complete frames.
    //! Notices a hangup. Never blocks.
    auto poll() -> std::vector<Message>;

    //! Whatever the plugin wrote to stderr since the last call (for the
    //! log). Never blocks.
    auto take_stderr() -> std::string;

    //! SIGTERM then, after the caller's grace, terminate() -> SIGKILL.
    auto request_stop() -> void; //!< SIGTERM, politely
    auto terminate() -> void;    //!< SIGKILL and reap

private:
    auto drain_fd(int fd, std::string& into) -> bool; //!< false on hangup
    auto flush_output() -> void;

    //! Reaps the child if it has exited, recording its wait status.
    auto reap_child(int options) -> void;

    int _stdin { -1 };  //!< our write end of the plugin's stdin
    int _stdout { -1 }; //!< our read end of the plugin's stdout
    int _stderr { -1 };
    pid_t _pid { -1 };
    bool _running { false };
    bool _reaped { false };
    int _wait_status { 0 }; //!< meaningful once _reaped

    std::string _incoming; //!< bytes read from stdout, awaiting whole frames
    std::string _outgoing; //!< frames queued but not yet written
    std::string _errors;   //!< stderr collected since the last take
};

}

#endif // SPICE_PLUGIN_PLUGINPROCESS_H
