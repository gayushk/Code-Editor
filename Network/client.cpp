#include "socket.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <csignal>
#include <cstring>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>


constexpr const char* SERVER_IP   = "127.0.0.1";
constexpr int         SERVER_PORT = 8484;

static struct termios g_orig_termios;

static void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    write(STDOUT_FILENO, "\033[?25h\033[0m", 9);
}

static void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0)
        throw std::runtime_error("tcgetattr failed");

    std::atexit(restore_terminal);

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        throw std::runtime_error("tcsetattr failed");
}

static void clear_screen() {
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
}

static void redraw(const std::string& content) {
    std::string frame;
    frame.reserve(content.size() + 256);
    frame += "\033[H";
    frame += "\033[7m  collab-editor  |  Ctrl-Q to quit  \033[0m\r\n";
    frame += "\r\n";

    for (char ch : content) {
        if (ch == '\n') frame += "\r\n";
        else            frame += ch;
    }

    frame += "\033[?25h";
    write(STDOUT_FILENO, frame.data(), frame.size());
}

static std::atomic<bool> g_running{true};

static void receiver_loop(SimpleNet::Socket& sock) {
    while (g_running.load()) {
        SimpleNet::RecvResult res;
        try {
            res = sock.receive(65536);
        } catch (...) {
            g_running.store(false);
            break;
        }

        if (res.status == SimpleNet::RecvStatus::Closed) {
            g_running.store(false);
            break;
        }
        if (res.status == SimpleNet::RecvStatus::WouldBlock)
            continue;

        std::string content(res.data.begin(), res.data.end());
        content.erase(content.find_last_not_of('\n') + 1);
        redraw(content);
    }
}

int main() {
    try {
        SimpleNet::Socket sock;
        sock.connect(SERVER_IP, SERVER_PORT);
        int flags = fcntl(sock.get_fd(), F_GETFL, 0);
        if (flags < 0) throw std::runtime_error("fcntl F_GETFL");
        if (fcntl(sock.get_fd(), F_SETFL, flags | O_NONBLOCK) < 0)
            throw std::runtime_error("fcntl F_SETFL");

        enable_raw_mode();
        clear_screen();
        write(STDOUT_FILENO, "\033[?25l\033[H", 9);
        std::thread receiver(receiver_loop, std::ref(sock));
        char ch;
        while (g_running.load()) {
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) continue;
            if (ch == ('q' & 0x1f)) {
                g_running.store(false);
                break;
            }
            try {
                sock.send(std::string_view(&ch, 1));
            } catch (...) {
                g_running.store(false);
                break;
            }
        }

        receiver.join();
        write(STDOUT_FILENO, "\r\n\033[0m", 6);
        restore_terminal();
        std::cout << "disconnected\n";

    } catch (const std::exception& e) {
        restore_terminal();
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
