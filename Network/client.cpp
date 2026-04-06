#include "socket.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <mutex>

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

struct Cursor {
	int row = 0;
	int col = 0;
};

static std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : content) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    lines.push_back(cur);
    return lines;
}

static int cursor_to_offset(const std::vector<std::string>& lines, const Cursor& cur) {
    int offset = 0;
    for (int r = 0; r < cur.row && r < (int)lines.size(); ++r)
        offset += (int)lines[r].size() + 1;
    offset += cur.col;
    return offset;
}

static void clamp_col(Cursor& cur, const std::vector<std::string>& lines) {
    if (cur.row < 0) cur.row = 0;
    if (cur.row >= (int)lines.size()) cur.row = (int)lines.size() - 1;
    int maxcol = (int)lines[cur.row].size();
    if (cur.col < 0)      cur.col = 0;
    if (cur.col > maxcol) cur.col = maxcol;
}

static void redraw(const std::string& content, const Cursor& cur) {
    std::string frame;
    frame.reserve(content.size() + 512);
    frame += "\033[H";
    frame += "\033[?25l";
    frame += "\033[7m  collab-editor  |  Ctrl-Q to quit  \033[0m\r\n";
    frame += "\r\n";

    for (char ch : content) {
        if (ch == '\n') frame += "\r\n";
        else            frame += ch;
    }
    frame += "\033[" + std::to_string(cur.row + 3) + ";" + std::to_string(cur.col + 1) + "H";
    frame += "\033[?25h";
    write(STDOUT_FILENO, frame.data(), frame.size());
}

static std::atomic<bool> g_running{true};
static std::string g_content;
static std::mutex g_content_mu;

static void receiver_loop(SimpleNet::Socket& sock) {
    struct pollfd pfd{sock.get_fd(), POLLIN, 0};
    while (g_running.load()) { 
        if (poll(&pfd, 1, 200) <= 0)
            continue;

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
        while (!content.empty() && content.back() == '\n') content.pop_back();
        {
            std::lock_guard<std::mutex> lock(g_content_mu);
            g_content = std::move(content);
        }
    }
}

enum class Key {
	Char, 
	Backspace, 
	ArrowLeft,
	ArrowRight,
	ArrowUp,
	ArrowDown,
	Quit,
	Enter,
	Unknown
};

struct KeyEvent {
	Key key;
	char ch;
};

static KeyEvent read_key() {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0)
        return {Key::Unknown, 0};
    if (c == ('q' & 0x1f))
        return {Key::Quit, 0};
    if (c == 127 || c == '\b')
        return {Key::Backspace, 0};
    if(c == '\r' || c == '\n') {
    	return {Key::Enter, 0};
    }
    if (c == '\033') {
        struct pollfd p{STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, 1) <= 0) return {Key::Unknown, 0};
        char seq[4] = {};
        int n = read(STDIN_FILENO, seq, sizeof(seq));
        if (n <= 0) return {Key::Unknown, 0};

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return {Key::ArrowUp,    0};
                case 'B': return {Key::ArrowDown,  0};
                case 'C': return {Key::ArrowRight, 0};
                case 'D': return {Key::ArrowLeft,  0};
            }
        }
        return {Key::Unknown, 0};
    }
    if (c >= 32 && c <= 126)
        return {Key::Char, c};

    return {Key::Unknown, 0};
}

static void send_insert(SimpleNet::Socket& sock, int offset, char ch) {
    std::string cmd = "I:" + std::to_string(offset) + ":" + ch + "\n";
    sock.send(cmd);
}

static void send_delete(SimpleNet::Socket& sock, int offset) {
    std::string cmd = "D:" + std::to_string(offset) + "\n";
    sock.send(cmd);
}

int main() {
    try {
        SimpleNet::Socket sock;
        sock.connect(SERVER_IP, SERVER_PORT);
        SimpleNet::set_nonblocking(sock.get_fd());

        enable_raw_mode();
        clear_screen();
        std::thread receiver(receiver_loop, std::ref(sock));
	Cursor cur;
	std::string local_content;
        while (g_running.load()) {
	    {
		    std::lock_guard<std::mutex> lock(g_content_mu);
	    	    if(local_content != g_content)
	    		local_content = g_content;
	    }
	    auto lines = split_lines(local_content);
	    clamp_col(cur, lines);
	    redraw(local_content, cur);
	    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
	    if (poll(&pfd, 1, 50) <= 0)
	        continue;
	    KeyEvent ev = read_key();
    	    switch(ev.key) {
	    	case Key::Quit:
			g_running.store(false);
			break;
		case Key::Char:
			{
			   int offset = cursor_to_offset(lines, cur);
			try{ 
			   send_insert(sock, offset, ev.ch);
			   local_content.insert(offset, 1, ev.ch);
			   cur.col++;
			} catch(...){ 
			   g_running.store(false);
			}
			break;
			}
		case Key::Backspace:
			{
			   if(cur.col > 0) {
			      int offset = cursor_to_offset(lines, cur) - 1;
				try{
			            send_delete(sock, offset);
			            cur.col--;
				} catch(...) {
			   	    g_running.store(false);
		        	}
			   } else if(cur.row > 0) {
			   	int offset = cursor_to_offset(lines, cur) - 1;
                        	try {
                                send_delete(sock, offset);
                                cur.row--;
                                cur.col = (int)lines[cur.row].size();
                        	} catch (...) { g_running.store(false); }
			   }
	    	           break;
			}
		case Key::Enter: {
                    int offset = cursor_to_offset(lines, cur);
                    try {
                        send_insert(sock, offset, '\n');
			cur.row++;
                        cur.col = 0;
                    } catch (...) { g_running.store(false); }
                    break;
		}
		case Key::ArrowLeft:
                    if (cur.col > 0) {
                        cur.col--;
                    } else if (cur.row > 0) {
                        cur.row--;
                        cur.col = (int)lines[cur.row].size();
                    }
                    break;		 
                case Key::ArrowRight:
		{	
                    int maxcol = (int)lines[cur.row].size();
                    if (cur.col < maxcol) {
                        cur.col++;
                    } else if (cur.row < (int)lines.size() - 1) {
                        cur.row++;
                        cur.col = 0;
                    }
                    break;
		}
                case Key::ArrowUp:
                    if (cur.row > 0) {
                        cur.row--;
                        clamp_col(cur, lines);
                    }
                    break;
                case Key::ArrowDown:
	            if (cur.row < (int)lines.size() - 1) {
                        cur.row++;
                        clamp_col(cur, lines);
                    }
                    break;
                case Key::Unknown:
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
