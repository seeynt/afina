#include "ServerImpl.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/logger.h>

#include <afina/Storage.h>
#include <afina/execute/Command.h>
#include <afina/logging/Service.h>

#include "protocol/Parser.h"

namespace Afina {
namespace Network {
namespace MTblocking {

// See Server.h
ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps, std::shared_ptr<Logging::Service> pl) : Server(ps, pl) {}

// See Server.h
ServerImpl::~ServerImpl() {}

// See Server.h
void ServerImpl::Start(uint16_t port, uint32_t n_accept, uint32_t n_workers) {
    _max_workers = n_workers;
    _logger = pLogging->select("network");
    _logger->info("Start mt_blocking network service");

    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, NULL) != 0) {
        throw std::runtime_error("Unable to mask SIGPIPE");
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_port = htons(port);       // TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any address

    _server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_server_socket == -1) {
        throw std::runtime_error("Failed to open socket");
    }

    int opts = 1;
    if (setsockopt(_server_socket, SOL_SOCKET, SO_REUSEADDR, &opts, sizeof(opts)) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket setsockopt() failed");
    }

    if (bind(_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket bind() failed");
    }

    if (listen(_server_socket, 5) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket listen() failed");
    }

    running.store(true);
    _thread = std::thread(&ServerImpl::OnRun, this);
}

// See Server.h
void ServerImpl::Stop() {
    running.store(false);
    shutdown(_server_socket, SHUT_RDWR);

    std::unique_lock<std::mutex> _lock(_mutex);
    for (auto& client_socket : _client_sockets) {
        shutdown(client_socket, SHUT_RD);
    }
}

// See Server.h
void ServerImpl::Join() {
    assert(_thread.joinable());
    _thread.join();

    std::unique_lock<std::mutex> _lock(_mutex);
    while (!_client_sockets.empty()) {
        _cv.wait(_lock);
    }
}

// See Server.h
void ServerImpl::OnRun() {
    // Here is connection state
    while (running.load()) {
        _logger->debug("waiting for connection...");

        // The call to accept() blocks until the incoming connection arrives
        int client_socket;
        struct sockaddr client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        if ((client_socket = accept(_server_socket, (struct sockaddr *)&client_addr, &client_addr_len)) == -1) {
            continue;
        }

        // Got new connection
        if (_logger->should_log(spdlog::level::debug)) {
            std::string host = "unknown", port = "-1";

            char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
            if (getnameinfo(&client_addr, client_addr_len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf),
                            NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
                host = hbuf;
                port = sbuf;
            }
            _logger->debug("Accepted connection on descriptor {} (host={}, port={})\n", client_socket, host, port);
        }

        // Configure read timeout
        {
            struct timeval tv;
            tv.tv_sec = 5; // TODO: make it configurable
            tv.tv_usec = 0;
            setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
        }

        // TODO: Start new thread and process data from/to connection
        std::unique_lock<std::mutex> _lock(_mutex);
        if (_client_sockets.size() < _max_workers) {
            _client_sockets.insert(client_socket);
            std::thread(&ServerImpl::Worker, this, client_socket).detach();
        }
        else {
            static const std::string msg = "TODO: start new thread and process memcached protocol instead";
            if (send(client_socket, msg.data(), msg.size(), 0) <= 0) {
                _logger->error("Failed to write response to client: {}", strerror(errno));
            }
            close(client_socket);
        }
    }

    close(_server_socket);
    // Cleanup on exit...
    _logger->warn("Network stopped");
}

void ServerImpl::Worker(int client_socket) {
    // - parser: parse state of the stream
    // - command_to_execute: last command parsed out of stream
    // - arg_remains: how many bytes to read from stream to get command argument
    // - argument_for_command: buffer stores argument
    std::size_t arg_remains = 0;
    Protocol::Parser parser;
    std::string argument_for_command;
    std::unique_ptr<Execute::Command> command_to_execute = nullptr;

    try {
        std::size_t parsed;
        int read_bytes = -1;
        char input[4096];

        while (read_bytes = read(client_socket, input, sizeof(input)) > 0) {
            _logger->debug("{} bytes have been processed ", read_bytes);

            while (read_bytes > 0) {
                if (!command_to_execute) {
                    //searching for command
                    if (parser.Parse(input, read_bytes, parsed)) {
                        _logger->debug("New command has been found in {} bytes - {}", parsed, parser.Name());
                        command_to_execute = parser.Build(arg_remains);

                        if (arg_remains > 0) {
                            arg_remains += 2;
                        }
                    }

                    if (!parsed) {
                        break;
                    }
                    else {
                        read_bytes -= parsed;
                        std::memmove(input, input + parsed, read_bytes);
                    }
                }
                else if (arg_remains > 0) {
                    //searching for additional args
                    int left_to_read = arg_remains < read_bytes ? arg_remains : read_bytes;
                    argument_for_command.assign(input, left_to_read);
                    std::memmove(input, input + left_to_read, read_bytes - left_to_read);

                    read_bytes -= left_to_read;
                    arg_remains -= left_to_read;
                }
                else if (arg_remains == 0) {
                    //time to execute!
                    _logger->debug("Command has been started");
                    std::string res;

                    if (argument_for_command.size()) {
                        argument_for_command.resize(argument_for_command.size() - 2);
                    }

                    command_to_execute->Execute(*pStorage, argument_for_command, res);

                    res += "\r\n";
                    if (send(client_socket, res.data(), res.size(), 0) <= 0) {
                        throw std::runtime_error("Failed to send response");
                    }

                    command_to_execute.reset();
                    argument_for_command.resize(0);
                    parser.Reset();
                }
            }

            if (!read_bytes) {
                _logger->debug("Connection closed");
            }
            else {
                throw std::runtime_error(std::string(strerror(errno)));
            }
        }
    } catch(std::runtime_error& e) {
        _logger->error("Error: {}", e.what());
    }

    close(client_socket);
    {
        std::unique_lock<std::mutex> _lock(_mutex);
        _client_sockets.erase(client_socket);

        if (_client_sockets.empty() && !running.load()) {
            _cv.notify_all();
        }
    }
}

} // namespace MTblocking
} // namespace Network
} // namespace Afina
