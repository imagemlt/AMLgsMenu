#include "ssh_command_client.h"

#include <libssh/libssh.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

SshCommandClient::SshCommandClient(std::string host, uint16_t port,
                                   std::string user, std::string password)
    : host_(std::move(host)), port_(port),
      user_(std::move(user)), password_(std::move(password)) {}

bool SshCommandClient::Send(const std::string &cmd, bool expect_reply, int timeout_ms) {
    std::vector<std::string> dummy;
    return Execute(cmd, expect_reply ? &dummy : nullptr, timeout_ms);
}

bool SshCommandClient::SendWithReply(const std::string &cmd,
                                     std::vector<std::string> &response,
                                     int timeout_ms) {
    return Execute(cmd, &response, timeout_ms);
}

bool SshCommandClient::Execute(const std::string &cmd, std::vector<std::string> *response,
                               int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ssh_session session = ssh_new();
    if (!session) {
        std::fprintf(stderr, "[AMLgsMenu] ssh_new failed\n");
        return false;
    }
    ssh_options_set(session, SSH_OPTIONS_HOST, host_.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port_);
    ssh_options_set(session, SSH_OPTIONS_USER, user_.c_str());
    int strict_host = 0;
    ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict_host);
    if (timeout_ms > 0) {
        long sec = std::max(1, timeout_ms / 1000);
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &sec);
    }
    int rc = ssh_connect(session);
    if (rc != SSH_OK) {
        std::fprintf(stderr, "[AMLgsMenu] ssh_connect: %s\n", ssh_get_error(session));
        ssh_free(session);
        return false;
    }
    rc = ssh_userauth_password(session, nullptr, password_.c_str());
    if (rc != SSH_AUTH_SUCCESS) {
        std::fprintf(stderr, "[AMLgsMenu] ssh_userauth_password failed: %s\n", ssh_get_error(session));
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }

    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        std::fprintf(stderr, "[AMLgsMenu] ssh_channel_new failed\n");
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    rc = ssh_channel_open_session(channel);
    if (rc != SSH_OK) {
        std::fprintf(stderr, "[AMLgsMenu] ssh_channel_open_session failed: %s\n", ssh_get_error(session));
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    rc = ssh_channel_request_exec(channel, cmd.c_str());
    if (rc != SSH_OK) {
        std::fprintf(stderr, "[AMLgsMenu] ssh_channel_request_exec failed: %s\n", ssh_get_error(session));
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }

    std::string collected;
    char buffer[512];
    while (true) {
        rc = ssh_channel_read_timeout(channel, buffer, sizeof(buffer), 0, timeout_ms);
        if (rc == SSH_ERROR) {
            std::fprintf(stderr, "[AMLgsMenu] ssh_channel_read failed: %s\n", ssh_get_error(session));
            break;
        }
        if (rc == SSH_AGAIN) {
            break;
        }
        if (rc == 0) {
            break;
        }
        if (response) {
            collected.append(buffer, rc);
        }
    }

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);

    if (response) {
        SplitLines(collected, *response);
    }
    return true;
}

void SshCommandClient::SplitLines(const std::string &text, std::vector<std::string> &out) {
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        out.push_back(line);
    }
}
