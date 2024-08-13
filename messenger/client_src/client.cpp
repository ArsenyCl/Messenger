#include <iostream>
#include <memory>
#include <boost/asio.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <memory>
#include <thread>
#include <algorithm>
#include <ctime>

struct talk_to_server;

static std::vector<std::string> split(const std::string &str, char split_by) {
    auto l = str.begin();
    auto r = str.begin();
    std::vector<std::string> res;
    while (l != str.end()) {
        while (*r == split_by && r != str.end()) {
            r++;
            l++;
        }
        while (*r != split_by && r != str.end()) {
            r++;
        }

        res.emplace_back(l, r);
        l = r;
    }
    return res;
}


struct talk_to_server {
    talk_to_server(boost::asio::io_service &service) :
            socket(service),
            already_read(0),
            last_rendered_l(0),
            last_rendered_r(0),
            last_rendered_message(),
            stop_all(false) {}

    void connect(boost::asio::ip::tcp::endpoint &ep) {
        socket.connect(ep);
    }

    void io_start() {
        std::string request;
        while (true) {
            std::cout << "\n\\s - sign in\n\\r - register\n\\q - quit messenger\n\nEnter command: ";
            std::getline(std::cin, request);
            if (request == "\\s") io_sign_in();
            else if (request == "\\r") io_reg();
            else if (request == "\\q") {
                stop();
                return;
            }
            else std::cout << incorrect_msg();
        }
    }

    void stop() {
        stop_all.store(true);
    }

    void ping() {
        time_t last_ping = time(nullptr);
        while (!stop_all.load()) {
            time_t current_time = time(nullptr);
            if (current_time - last_ping >= 10) {
                last_ping = current_time;
                write("ping");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    static const char *start_msg() {
        return "Welcome to Oksiliz messenger!\n";
    }

    static const char *end_msg() {
        return "Thank you for your visit! See you later in Oksiliz messenger!\n";
    }

    static const char *incorrect_msg() {
        return "Incorrect input!\n";
    }

private:

    void io_sign_in() {
        while (true) {
            std::cout << "\nWrite your login and password\n";
            std::cout << "\\b - back\n";
            std::cout << "\nLogin: ";
            std::getline(std::cin, login);
            if (login == "\\b") return;
            std::cout << "Password: ";
            std::getline(std::cin, password);
            if (password == "\\b") return;
            if (!check_login_and_password_correctness(login, password))
                continue;
            std::string answer = write_read("signin," + login + "," + password);
            std::cout << answer << '\n';
            if (answer.find("You are logged in as '" + login + "'") == 0) {
                io_dialogs();
                return;
            }
        }
    }

    void io_reg() {
        std::cout << "\nCreate your login and password\n";

        while (true) {
            std::cout << "\n\\b - back\n";

            std::string repeat_password;
            std::cout << "\nLogin: ";
            std::getline(std::cin, login);
            if (login == "\\b") return;
            std::cout << "Password: ";
            std::getline(std::cin, password);
            if (password == "\\b") return;
            std::cout << "Repeat password: ";
            std::getline(std::cin, repeat_password);
            if (repeat_password == "\\b") return;

            if (!check_login_and_password_correctness(login, password))
                continue;

            if (password != repeat_password) {
                std::cout << "Passwords are different!\n";
                continue;
            }

            std::string answer = write_read("reg," + login + "," + password);
            std::cout << answer << '\n';
            if (answer.find("Account registered successfully!") == 0) {
                io_dialogs();
                return;
            }
        }
    }

    void io_dialogs() {
        std::string request;
        int num;
        while (true) {
            render_dialogs();
            std::cout << "\n\\q - log out\n\\n - new dialog\n";
            std::cout << dials;
            std::cout << "\nEnter command: ";
            std::getline(std::cin, request);

            if (request == "\\q") {
                if (io_log_out())
                    return;
            } else if (request == "\\n") {
                io_new_dialog();
            } else {
                try {
                    num = std::stoi(request);
                } catch (...) {
                    std::cout << '\n' << incorrect_msg();
                    continue;
                }
                if (num > 0 && num < sv.size() && dials != "You do not have any dialogs right now. Start some new dialogs using '\\n'\n") {
                    io_messages(split(sv[num - 1], ' ')[2]);
                } else {
                    std::cout << '\n' << incorrect_msg();
                }
            }
        }
    }

    void io_messages(const std::string &other_login) {
        size_t num_of_messages = 30;
        size_t current_page = 0;
        while (true) {
            std::cout << "\nDialog with " + other_login + "\n\n";
            if (current_page > 0) {
                std::cout << "\\l - to last page\n\\n - next page\n";
            }
            if (current_page == 0) {
                std::cout << "\\u - update\n";
            }
            std::cout << "\\p - previous page\n\\q - to list of dialogs\n";


            if (render_messages(other_login, current_page * num_of_messages + 1, (current_page + 1) * num_of_messages + 1, false))
                std::cout << last_rendered_message;

            if (current_page == 0) {
                std::cout << "Send message: ";
            }
            std::string message;
            std::getline(std::cin, message);

            if (message == "\\n") {
                if (current_page == 0)
                    std::cout << "\nThis is the last page!\n";
                else
                    current_page++;
                continue;
            }

            if (message == "\\u") {
                if (current_page > 0) {
                    std::cout << "\nOpen the last page first!\n";
                } else {
                    std::cout << "\nUpdating dialog...\n";
                    render_messages(other_login, current_page * num_of_messages + 1, (current_page + 1) * num_of_messages + 1, true);
                }
                continue;
            }

            if (message == "\\l") {
                if (current_page == 0) {
                    std::cout << "\nYou are already on the last page!\n";
                } else {
                    current_page = 0;
                }
                continue;
            }

            if (message == "\\p") {
                current_page++;
                if (!render_messages(other_login, current_page * num_of_messages + 1, (current_page + 1) * num_of_messages + 1, false)) {
                    std::cout<<"\nThis is the first page!\n";
                    current_page--;
                }
                continue;
            }

            if (message == "\\q")
                return;


            if (message.find('\\') == 0) {
                std::cout<<"\nUnknown command: " + message + '\n';
                continue;
            }

            if (message.find('\3') != std::string::npos) {
                std::cout<<"\n Character '\\3' is illegal in message!\n";
                continue;
            }

            if (current_page == 0) {
                if (!message.empty())
                    send_message(message, other_login);
                render_messages(other_login, current_page * num_of_messages + 1, (current_page + 1) * num_of_messages + 1, true);
            } else {
                std::cout<<"\nIncorrect input!\n";
            }


        }
    }

    void io_new_dialog() {
        while(true) {
            std::cout<<"\n\\b - back\nEnter a login of recipient\nEnter login: ";
            std::string other_login;
            std::getline(std::cin, other_login);

            if (other_login == "\\b")
                return;

            if (other_login.find('\\') == 0) {
                std::cout<<"\nUnknown command: " + other_login + '\n';
                continue;
            }

            if (other_login.size() < 5 || other_login.size() > 30) {
                std::cout<<"Incorrect login size: "<<other_login.size()<<'\n';
                continue;
            }

            std::string tmp_ans = write_read("find login," + other_login);
            if (tmp_ans == "Login found") {
                io_messages(other_login);
                return;
            } else if (tmp_ans == "No such login")
                std::cout<<"\nLogin '" + other_login + "' not found\n";
        }
    }

    bool io_log_out() {
        std::string request;
        while (true) {
            std::cout << "\nAre you sure, that you want to log out?\n\\c - cancel\n\\q - log out\nEnter command: ";
            std::getline(std::cin, request);
            if (request == "\\c") return false;
            else if (request == "\\q") {
                write("logout");
                std::cout<<"\n\nSee you later '" + login + "' !\n";
                login.clear();
                password.clear();
                dials.clear();
                last_rendered_message.clear();
                last_rendered_r = 0;
                last_rendered_l = 0;
                return true;
            }
            else std::cout << incorrect_msg();
        }
    }

    std::string buffered_read() {
        std::string res;
        bool found_stop = false;
        size_t pos;
        char bf[max_msg];
        size_t have_read = 0;
        std::lock_guard lg(m);
        std::string nxt = "next";
        while (!found_stop) {
            socket.write_some(boost::asio::buffer(nxt + '\3'));
            while (have_read < 1024 && !found_stop) {
                if (socket.available()) {
                    have_read += socket.read_some(boost::asio::buffer(bf + have_read, max_msg - have_read));
                    pos = std::find(bf, bf + have_read, '\3') - bf;
                    found_stop = pos < have_read;
                }
            }
            res += std::string(bf, pos);
        }
        return res;
    }

    std::string read_answer() {
        bool found_enter = false;
        size_t pos;
        while (!found_enter) {
            if (socket.available()) {
                already_read += socket.read_some(boost::asio::buffer(buffer + already_read, max_msg - already_read));
                pos = std::find(buffer, buffer + already_read, '\3') - buffer;
                found_enter = pos < already_read;
            }
        }
        std::string answer(buffer, pos);
        std::copy(buffer + already_read, buffer + max_msg, buffer);
        already_read -= (pos + 1);

        return answer;
    }

    void write(const std::string &str) {
        std::lock_guard lg(m);
        socket.write_some(boost::asio::buffer(str + '\3'));
    }

    void render_dialogs() {
        std::string tmp_ans = write_read("dialogs");
        if (tmp_ans == "bw") {
            dials = buffered_read();
            sv = split(dials, '\n');
        } else
            std::cerr << "Unexpected server answer!\nexpected: bw or on date\ngot: " << tmp_ans << '\n';
    }

    bool render_messages(const std::string &other_login, size_t from, size_t to, bool update) {
        if (!update && other_login == last_rendered_login && from == last_rendered_l && to == last_rendered_r) {
            return true;
        }

        std::string tmp_ans = write_read("messages," + other_login + "," + std::to_string(from) + "," + std::to_string(to));
        if (tmp_ans == "bw") {
            last_rendered_message = buffered_read();
            last_rendered_login = other_login;
            last_rendered_l = from;
            last_rendered_r = to;
            return true;
        } else if (tmp_ans == "empty") {
            return false;
        } else {
            std::cerr << "Unexpected server answer!\nexpected: bw or empty\ngot: " + tmp_ans + '\n';
            return false;
        }
    }


    std::string write_read(const std::string& msg) {
        std::lock_guard lg(m);
        socket.write_some(boost::asio::buffer(msg + '\3'));
        return read_answer();
    }

    void send_message(const std::string& message, const std::string& other_login) {
        size_t to_write = max_msg - 4;
        auto it = message.begin();
        while (it != message.end()) {
            std::string tmp_ans = write_read("bw " + std::string(it, std::min(it + to_write, message.end())));
            it = std::min(it + to_write, message.end());
            if (tmp_ans != "next")
                std::cerr<<"Unexpected server answer!\nexpected: next\ngot: " + tmp_ans + '\n';
        }
        write("send message," + other_login);
    }

    static bool check_login_and_password_correctness(const std::string &log, const std::string &pass) {
        if (log.size() < 5) {
            std::cout << "Login must contain at least 5 characters!\n";
            return false;
        }

        if (pass.size() < 5) {
            std::cout << "Password must contain at least 5 characters!\n";
            return false;
        }

        if (log.size() > 30) {
            std::cout << "Login must contain at most 30 characters!\n";
            return false;
        }

        if (pass.size() > 100) {
            std::cout << "Password must contain at most 100 characters!\n";
            return false;
        }

        if (log.find('\n') != std::string::npos || pass.find('\n') != std::string::npos) {
            std::cout << "Character '\\n' is illegal!\n";
            return false;
        }

        if (log.find(',') != std::string::npos || pass.find(',') != std::string::npos) {
            std::cout << "Character ',' is illegal!\n";
            return false;
        }

        if (log.find('\n') != std::string::npos || pass.find('\n') != std::string::npos) {
            std::cout << "Character '\\n' is illegal!\n";
            return false;
        }

        if (log.find('\3') != std::string::npos || pass.find('\3') != std::string::npos) {
            std::cout << "Character '\\3' is illegal!\n";
            return false;
        }
        return true;
    }



    constexpr static size_t max_msg = 1024;
    boost::asio::ip::tcp::socket socket;


    size_t already_read;
    char buffer[max_msg];


    std::string login;
    std::string password;


    std::string dials;
    std::vector<std::string> sv;

    std::string last_rendered_message;
    std::string last_rendered_login;
    size_t last_rendered_l;
    size_t last_rendered_r;

    std::atomic<bool> stop_all;

    std::mutex m;

};

struct messenger_client {
    void run_client(const std::string &server_ip) {
        using namespace boost::asio::ip;
        tcp::endpoint ep(address::from_string(server_ip), 1234);

        talk_to_server client(service);
        std::cout << talk_to_server::start_msg();
        try {
            client.connect(ep);
            std::jthread frontend([&client](){
                try {
                    client.io_start();
                } catch (const std::exception &e) {
                    client.stop();
                    std::cerr<<e.what()<<'\n';
                }
            });
            std::jthread ping([&client](){
                client.ping();
            });
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
        }
        std::cout << talk_to_server::end_msg();
    }

    boost::asio::io_service service;
};

int main() {
    messenger_client client;
    client.run_client("127.0.0.1");
}