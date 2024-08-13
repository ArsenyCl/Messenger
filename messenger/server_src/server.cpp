#include <iostream>
#include <memory>
#include <boost/asio.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <memory>
#include <thread>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <pqxx/pqxx>

pqxx::connection *cn;

struct bad_request_exception : std::exception {
    const char *what() const noexcept override {
        return message.c_str();
    }

    bad_request_exception(const std::string &m) : message(m) {}

private:
    std::string message;
};

struct buffered_writer {
    buffered_writer() = default;

    buffered_writer(std::string &&str) : text(std::move(str)), it(text.begin()) {}

    void write_next(boost::asio::ip::tcp::socket &sock, size_t size) {
        if (it + size < text.end()) {
            sock.write_some(boost::asio::buffer(std::string_view(it, it + size)));
            it += size;
        } else {
            sock.write_some(boost::asio::buffer(std::string_view(it, text.end())));
            it = text.end();
        }
    }

    bool has_next() {
        return it < text.end();
    }

private:
    std::string text;
    std::string::iterator it;
};


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


struct talk_to_client : std::enable_shared_from_this<talk_to_client> {
    talk_to_client(boost::asio::io_service &service) : socket(service), already_read(0), last_ping(time(nullptr)) {}

    void answer_to_client() {
        if (time(nullptr) - last_ping > 100) {
            if (!login.empty()) {
                logout();
            }
            stop();
            return;
        }

        try {
            read_request();
            process_request();
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            stop();
        }
    }


    boost::asio::ip::tcp::socket &sock() {
        return socket;
    }

private:
    void write(const std::string &msg) {
        socket.write_some(boost::asio::buffer(msg + '\3'));
    }

    void read_request() {
        if (socket.available())
            already_read += socket.read_some(boost::asio::buffer(buffer + already_read, max_msg - already_read));
    }

    void process_request() {
        size_t pos = std::find(buffer, buffer + already_read, '\3') - buffer;
        bool found_enter = pos < already_read;
        if (!found_enter)
            return;
        std::string request(buffer, pos);
        std::cout << "new request: " << request << '\n';
        std::copy(buffer + already_read, buffer + max_msg, buffer);
        already_read -= (pos + 1);

        if (request.find("signin,") == 0) {
            sign_in(request);
        } else if (request.find("reg,") == 0) {
            reg(request);
        } else if (request == "dialogs") {
            dialogs();
        } else if (request.find("messages,") == 0) {
            messages(request);
        } else if (request == "logout") {
            logout();
        } else if (request.find("find login,") == 0) {
            find_login(request);
        } else if (request.find("bw ") == 0) {
            message += std::string_view(request.begin() + 3, request.end());
            write("next");
        } else if (request.find("send message,") == 0) {
            send_message(request);
        } else if (request == "next") {
            if (bw.has_next()) {
                bw.write_next(sock(), max_msg);
            } else
                throw bad_request_exception("Asking for write_next, but bw dont have next!");
        }  else if (request == "ping") {
            ping();
        } else {
            std::cout << "bad request: " << request<<'\n';
            stop();
        }
    }

    void stop() {
        std::cout<<"Closing socket\n";
        socket.close();
    }

    void sign_in(const std::string &request) {
        if (!login.empty()) {
            throw bad_request_exception("Login: ' " + login + "' is not empty, double register!");
        }
        pqxx::nontransaction N(*cn);
        auto sv = split(request, ',');

        if (sv.size() != 3
            || sv[1].size() < 5 || sv[2].size() < 5
            || sv[1].size() > 30 || sv[2].size() > 200
            || sv[1].find('\n') != std::string::npos || sv[2].find('\n') != std::string::npos
            || sv[1].find(',') != std::string::npos || sv[2].find(',') != std::string::npos
            || sv[1].find('\3') != std::string::npos || sv[2].find('\3') != std::string::npos) {
            throw bad_request_exception("Unexpected input of login and password from user!");
        }
        pqxx::result r = N.exec("SELECT id, login, password FROM USERS WHERE LOGIN='" + sv[1] + "';");
        N.commit();

        if (r.size() > 1) {
            throw bad_request_exception("There more then 1 login '" + sv[1] + "' in database Users!");
        }
        if (r.empty()) {
            write("Login: " + sv[1] + " does not exists");
            return;
        }
        if (r[0][2].as<std::string>() != sv[2]) {
            write("Wrong password");
            return;
        }
        login = sv[1];
        id = r[0][0].as<int>();

        pqxx::work W(*cn);
        W.exec("UPDATE users SET online = online + 1 where id = " + std::to_string(id) + ";");
        W.commit();

        write("You are logged in as '" + login + "'");
    }

    void reg(const std::string &request) {
        if (!login.empty()) {
            throw bad_request_exception("Login: ' " + login + "' is not empty, double register!");
        }
        pqxx::nontransaction N(*cn);
        auto sv = split(request, ',');

        if (sv.size() != 3 || sv[1].size() < 5 || sv[2].size() < 5 || sv[1].size() > 100 || sv[2].size() > 100) {
            throw bad_request_exception(
                    "Unexpected num of parameters in reg\nExpected: 3\nGot: " + std::to_string(sv.size()));
        }


        pqxx::result r = N.exec("SELECT id FROM USERS WHERE LOGIN='" + sv[1] + "';");

        if (!r.empty()) {
            write("Login '" + sv[1] + "' already exists!");
            return;
        }
        N.commit();
        pqxx::work W(*cn);

        pqxx::result num_of_users = W.exec("SELECT COUNT (*) AS N FROM USERS;");

        int num = num_of_users.empty() ? 1 : num_of_users[0][0].as<int>() + 1;

        W.exec("INSERT INTO USERS (ID, LOGIN, PASSWORD, ONLINE, UPD) \
               VALUES (" + std::to_string(num) + ", '" + sv[1] + "', '" + sv[2] + "', 1, TRUE);");
        W.commit();

        login = sv[1];
        id = num;
        write("Account registered successfully!");
    }

    void dialogs() {
        if (login.empty()) {
            throw bad_request_exception("Empty login in dialogs");
        }
        pqxx::nontransaction N(*cn);

        std::string str_id = std::to_string(id);
        pqxx::result dial = N.exec("SELECT COALESCE(TT.N, 0) as N, USERS.login, TT.DATETIME, USERS.ONLINE  FROM (  \n"
                                   "  SELECT TBL.N as N, R.ID as id, R.MMDT as DATETIME\n"
                                   "    FROM (\n"
                                   "    SELECT COUNT(*) as N, from_id as id \n"
                                   "    FROM MESSAGES\n"
                                   "    WHERE to_id = " + str_id + " and is_read = FALSE\n"
                                                                   "    GROUP BY from_id\n"
                                                                   "  ) AS TBL\n"
                                                                   "  FULL JOIN (\n"
                                                                   "      SELECT id, max(mdt) as mmdt\n"
                                                                   "      FROM (\n"
                                                                   "        SELECT from_id AS ID, max(DT) as MDT \n"
                                                                   "    from messages\n"
                                                                   "    WHERE to_id = " + str_id + "\n"
                                                                                                   "    GROUP BY from_id\n"
                                                                                                   "    UNION all\n"
                                                                                                   "    SELECT to_id AS ID, max(DT) as MDT \n"
                                                                                                   "    from messages\n"
                                                                                                   "    WHERE from_id = " +
                                   str_id + "\n"
                                            "    GROUP BY to_id\n"
                                            "      ) AS A\n"
                                            "  group by ID\n"
                                            "    ) AS R\n"
                                            "  ON TBL.ID = R.ID\n"
                                            ") AS TT\n"
                                            "INNER JOIN USERS\n"
                                            "ON TT.ID = USERS.ID\n"
                                            "ORDER BY TT.DATETIME DESC;");
        N.commit();
        pqxx::work W(*cn);
        W.exec("UPDATE users SET upd = TRUE where id=" + std::to_string(id) + ";");
        W.commit();
        std::string to_write;
        size_t i = 1;
        auto online_to_str = [](int online) {
            return online > 0 ? "online" : "offline";
        };
        for (auto &&row: dial) {
            to_write +=
                    std::to_string(i) + " - " + row[1].as<std::string>() + " " + row[2].as<std::string>() + " [" +
                    row[0].as<std::string>() + "] " + online_to_str(row[3].as<int>()) + "\n";
            i++;
        }
        if (dial.empty())  {
            to_write += "You do not have any dialogs right now. Start some new dialogs using '\\n'\n";
        }

        to_write += '\3';
        bw = buffered_writer(std::move(to_write));
        write("bw");
    }

    void send_message(const std::string &request) {
        auto sv = split(request, ',');
        if (sv.size() != 2) {
            throw bad_request_exception(
                    "Unexpected num of parameters in send_message\nExpected: 2\nGot: " + std::to_string(sv.size()));
        }
        pqxx::nontransaction N(*cn);
        std::string message_id = std::to_string(N.exec1("SELECT COUNT(*) FROM MESSAGES")[0].as<long long>() + 1);
        std::string to_id = N.exec1("SELECT id FROM users WHERE login='" + sv[1] + "';")[0].as<std::string>();
        N.commit();

        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);
        std::ostringstream current_time;
        current_time << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

        pqxx::work W(*cn);

        W.exec("insert into messages(id, dt, from_id, to_id, txt, parent_id, is_read) \
               VALUES \
               (" + message_id + ", '" + current_time.str() + "', " + std::to_string(id) + ", " + to_id + ", '" +
               message + "', 0, FALSE);");
        message.clear();
        W.exec("UPDATE users SET upd = FALSE where id=" + to_id + ";");
        W.commit();
    }

    void find_login(const std::string &request) {
        auto sv = split(request, ',');
        if (sv.size() != 2)
            throw bad_request_exception(
                    "Unexpected num of parameters in find_login\nExpected: 2\nGot: " + std::to_string(sv.size()));

        pqxx::nontransaction N(*cn);
        auto r = N.exec("SELECT login FROM users WHERE login='" + sv[1] + "';");
        N.commit();

        if (r.empty()) {
            write("No such login");
        } else if (r.size() == 1) {
            write("Login found");
        } else {
            throw bad_request_exception("There more then 1 login '" + sv[1] + "' in database Users!");
        }
    }

    void messages(const std::string &request) {
        auto sv = split(request, ',');
        if (sv.size() != 4) {
            throw bad_request_exception(
                    "Unexpected num of parameters in messages\nExpected: 4\nGot: " + std::to_string(sv.size()));
        }


        pqxx::nontransaction N(*cn);
        auto other_id_r = N.exec("SELECT id FROM USERS WHERE LOGIN='" + sv[1] + "';");

        if (other_id_r.size() > 1) {
            throw bad_request_exception("There more then 1 login '" + sv[1] + "' in database Users!");
        }

        if (other_id_r.empty()) {
            throw bad_request_exception("No such login '" + sv[1] + "' in database Users!");
        }


        auto other_id = other_id_r[0][0].as<std::string>();
        std::string this_id = std::to_string(id);
        pqxx::result messages = N.exec("SELECT x.rn, x.dt, x.from_id, x.txt, x.parent_id FROM  (\n"
                                       "\tSELECT ROW_NUMBER() over(ORDER BY DT) AS RN , dt, from_id, txt, parent_id \n"
                                       "\tFROM messages\n"
                                       "\twhere (from_id = " + this_id + " and to_id = " + other_id + ") or (to_id = " +
                                       this_id + " and from_id = " + other_id + ")\n"
                                                                                ") as x\n"
                                                                                "where " + sv[2] +
                                       " <= x.rn AND  x.rn <" + sv[3] + ";");
        N.commit();
        pqxx::work W(*cn);
        W.exec("UPDATE messages SET is_read = true where from_id = " + other_id + " AND to_id = " + this_id + ";");
        W.commit();
        if (messages.empty()) {
            write("empty");
            return;
        }

        std::string to_write;
        auto id_to_name = [&sv, this](int current_id) {
            return id == current_id ? "You" : sv[1];
        };

        for (auto &&row: messages) {
            to_write += id_to_name(row[2].as<int>()) + " at " + split(row[1].as<std::string>(), ' ')[1] + ": " +
                        row[3].as<std::string>() + '\n';
        }
        to_write += '\3';
        bw = buffered_writer(std::move(to_write));
        write("bw");
    }


    void logout() {
        pqxx::work W(*cn);
        W.exec("UPDATE users SET online = online - 1 where id=" + std::to_string(id) + ";");
        W.commit();
        id = 0;
        login.clear();
    }

    void ping() {
        last_ping = time(nullptr);
    }

    constexpr static size_t max_msg = 1024;
    boost::asio::ip::tcp::socket socket;

    size_t already_read;
    char buffer[max_msg];

    std::string login;
    int id;

    buffered_writer bw;

    std::string message;

    time_t last_ping;
};

struct messenger_server {

    using client_ptr = std::shared_ptr<talk_to_client>;

    void run_server() {
        std::jthread accept_thread([this]() {
            this->accept_thread();
        });
        std::jthread handle_clients_thread([this]() {
            this->handle_clients_thread();
        });
    }

    void handle_clients_thread() {
        using namespace boost::asio::ip;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            boost::recursive_mutex::scoped_lock lk(cs);
            for (auto &&client: clients) {
                client->answer_to_client();
            }
            size_t sz = clients.size();
            clients.erase(std::remove_if(clients.begin(), clients.end(), [](const auto& client){return !client->sock().is_open();}), clients.end());
            if (sz - clients.size() > 0)
                std::cout<<"Clients removed: "<<sz - clients.size()<<'\n';
        }
    }

    void accept_thread() {
        using namespace boost::asio::ip;
        tcp::acceptor acceptor(service, tcp::endpoint(tcp::v4(), 1234));
        while (true) {
            client_ptr new_client(new talk_to_client(service));
            acceptor.accept(new_client->sock());
            std::cout << "new client! client num: " << clients.size() << '\n';
            boost::recursive_mutex::scoped_lock lk(cs);
            clients.push_back(new_client);
        }
    }

    boost::asio::io_service service;
    std::vector<client_ptr> clients;
    boost::recursive_mutex cs;
};

static void try_to_exec(pqxx::connection &C, const char *sql) {
    pqxx::work W(C);
    try {
        W.exec(sql);
        W.commit();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
}

int main() {

    constexpr char *users_table = "CREATE TABLE IF NOT EXISTS USERS("  \
      "ID INT ," \
      "LOGIN VARCHAR(30)," \
      "PASSWORD VARCHAR(200)," \
      "ONLINE INT," \
      "UPD BOOL);";

    constexpr char *messages_table = "CREATE TABLE IF NOT EXISTS MESSAGES("  \
      "ID BIGINT ," \
      "DT TIMESTAMP," \
      "FROM_ID INT," \
      "TO_ID INT," \
      "TXT TEXT," \
      "PARENT_ID BIGINT," \
      "IS_READ BOOL);";


    try {
        pqxx::connection C("dbname = presn user = presn password = 22042004a \
      hostaddr = 127.0.0.1 port = 5432");
        cn = &C;
        if (C.is_open()) {
            std::cout << "Opened database successfully: " << C.dbname() << std::endl;
        } else {
            std::cout << "Can't open database" << std::endl;
            return 1;
        }


        /* Create a transactional object. */
        try_to_exec(C, users_table);
        try_to_exec(C, messages_table);

        std::cout << "Tables created successfully" << std::endl;
//
//        try_to_exec(C, "insert into users(id, login, password, online, upd)\n"
//                       "VALUES\n"
//                       "(1, 'Петя', '228225', 0, TRUE),\n"
//                       "(2, 'Вася', '228225', 0, TRUE),\n"
//                       "(3, 'Толя', '228225', 0, TRUE),\n"
//                       "(4, 'Чарли', '228225', 0, TRUE),\n"
//                       "(5, 'Паша', '228225', 0, TRUE);\n");
//
//        try_to_exec(C, "insert into messages(id, dt, from_id, to_id, txt, parent_id, is_read)\n"
//                       "VALUES\n"
//                       "(1, '2024-02-28 11:19:41', 1, 2, 'Привет', 0, FALSE),\n"
//                       "(2, '2024-02-28 11:20:03', 1, 2, 'Как дела?', 0, FALSE),\n"
//                       "(3, '2024-02-28 11:20:06', 2, 3, 'Ку', 0, True),\n"
//                       "(4, '2024-02-28 11:22:13', 3, 2, 'Сап', 0, True),\n"
//                       "(5, '2024-02-28 11:22:46', 2, 3, 'Мне опять Петя пишет...', 0, FALSE),\n"
//                       "(6, '2024-02-29 7:45:13', 2, 4, 'Привет', 0, FALSE),\n"
//                       "(7, '2024-02-29 9:56:58', 1, 2, 'Хули ты игноришь', 0, FALSE);");


        messenger_server server;
        server.run_server();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}