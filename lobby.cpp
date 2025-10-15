#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include <chrono>

using json = nlohmann::json;
using namespace std;

const int LOBBY_PORT = 12000;
const char* DB_FILE = "users.json";
mutex db_m;

struct User {
    string password;
    int login_count = 0;
    int experience = 0;
    bool online = false;
    chrono::steady_clock::time_point last_seen = chrono::steady_clock::now();
};

unordered_map<string, User> users;

void load_db() {
    lock_guard<mutex> g(db_m);
    ifstream in(DB_FILE);
    if (!in.good()) {
        cout<<"[DEBUG] Database Failed to open\n";
        return;
    }
    json j;
    try {
        in >> j;
        for (auto it = j.begin(); it != j.end(); ++it) {
            User u;
            u.password = it.value().value("password", "");
            u.login_count = it.value().value("login_count", 0);
            u.experience = it.value().value("experience", 0);
            u.online = it.value().value("online", false);
            users[it.key()] = u;
        }
    } catch (...) {
        cerr << "Failed to parse DB file\n";
    }
    cout<<"[DEBUG] Load Database Successful\n";
}

void save_db() {
    cout << "[DEBUG] save_db called\n";
    ofstream f("users.json");
    if (!f.is_open()) {
        cerr << "[DEBUG] ERROR: Cannot open users.json for writing!\n";
        return;
    }
    cout << "[DEBUG] users.json opened for writing\n";

    json j;
    for (auto &[k,v] : users) {
        j[k] = {{"password", v.password},
                {"login_count", v.login_count},
                {"experience", v.experience},
                {"online", v.online}};
    }
    cout << "[DEBUG] JSON prepared, writing to file...\n";

    f << j.dump(4) << endl;
    f.close();
    cout << "[DEBUG] save_db finished\n";
}

ssize_t recv_line(int fd, string &out) {
    out.clear();
    char buf[1024];
    while (true) {
        ssize_t n = recv(fd, buf, 1, 0);
        if (n <= 0) return n;
        if (buf[0] == '\n') break;
        out.push_back(buf[0]);
    }
    return (ssize_t)out.size();
}

bool send_json(int fd, const json &j) {
    string s = j.dump();
    s.push_back('\n');
    ssize_t total = 0;
    const char* data = s.c_str();
    ssize_t tosend = s.size();
    while (total < tosend) {
        ssize_t n = send(fd, data + total, tosend - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

void handle_client(int client_fd) {   
    string username;
    try{
        while (true) {
            string line;
            ssize_t r = recv_line(client_fd, line);
            if (r <= 0){
                cout << "[DEBUG] Client socket closed, marking offline\n";
                if (!users.empty()) {
                    lock_guard<mutex> g(db_m);
                    users[username].online = false;
                    save_db();
                }
                close(client_fd);
                break;
            }
            json req;
            try { req = json::parse(line); } catch (...) { 
                json res = { {"status","ERR"}, {"detail","BAD_JSON"} };
                send_json(client_fd, res);
                continue;
            }
            string cmd = req.value("cmd", "");
            cout << "[LOBBY] Received cmd=" << cmd << " user=" << req.value("username", "") << endl;
            if (cmd == "REGISTER") {
                cout<<"[DEBUG] Start Registering...\n";
                username = req.value("username", "");
                string password = req.value("password", "");
                lock_guard<mutex> g(db_m);
                if (users.find(username) != users.end()) {
                    send_json(client_fd, json{{"status","ERR"},{"detail","USER_EXISTS"}});
                    cout<<"[DEBUG] User Already Exists\n";
                } else {
                    User u; u.password = password; u.login_count = 0; u.experience = 0; u.online = false;
                    users[username] = u;
                    save_db();
                    send_json(client_fd, json{{"status","OK"},{"detail","REGISTER_SUCCESS"}});
                    cout<<"[DEBUG] User Registered successfully\n";
                }
            } else if (cmd == "LOGIN") {
                string username = req.value("username", "");
                string password = req.value("password", "");
                lock_guard<mutex> g(db_m);
                auto it = users.find(username);
                if (it == users.end()) {
                    send_json(client_fd, json{{"status","ERR"},{"detail","NO_SUCH_USER"}});
                } else if (it->second.password != password) {
                    send_json(client_fd, json{{"status","ERR"},{"detail","WRONG_PASSWORD"}});
                } else {
                    if(it->second.online==false){
                        it->second.login_count += 1;
                        it->second.online = true;
                        save_db();
                        send_json(client_fd, json{{"status","OK"},{"detail","LOGIN_SUCCESS"},
                                                {"login_count", it->second.login_count},
                                                {"experience", it->second.experience}});
                    }else{
                        send_json(client_fd, json{{"status","TAKEN"},{"detail","LOGIN_FAIL"},
                                                {"login_count", it->second.login_count},
                                                {"experience", it->second.experience}});
                    }
                }
            } else if (cmd == "LOGOUT") {
                string username = req.value("username", "");
                lock_guard<mutex> g(db_m);
                auto it = users.find(username);
                if (it != users.end()) {
                    it->second.online = false;
                    save_db();
                }
                send_json(client_fd, json{{"status","OK"},{"detail","LOGOUT_SUCCESS"}});
            } else if (cmd == "STATUS") {
                string username = req.value("username", "");
                json extra = req.value("extra", json::object());
                lock_guard<mutex> g(db_m);
                auto it = users.find(username);
                if (it == users.end()) {
                    send_json(client_fd, json{{"status","ERR"},{"detail","NO_SUCH_USER"}});
                } else {
                    int xp = extra.value("xp", 0);
                    it->second.last_seen = chrono::steady_clock::now();
                    it->second.online = true;
                    save_db();
                    send_json(client_fd, json{{"status","OK"},{"detail","STATUS_UPDATED"},{"experience", it->second.experience}});
                }
            } else if (cmd == "ONLINE_STATUS") {
                string query_user = req.value("username", "");
                lock_guard<mutex> g(db_m);
                bool on = (users.find(query_user) != users.end()) ? users[query_user].online : false;
                send_json(client_fd, {{"status","OK"},{"online", on}});
            }else {
                send_json(client_fd, json{{"status","ERR"},{"detail","UNKNOWN_CMD"}});
            }
        }
    } catch (...){
        cout << "[ERROR] Exception in client handler\n";
        if (!users.empty()) {
            lock_guard<mutex> g(db_m);
            users[username].online = false;
            save_db();
        }
    }
    close(client_fd);
}

int main() {

    thread([&](){
        while (true) {
            this_thread::sleep_for(chrono::seconds(5));  
            lock_guard<mutex> g(db_m);
            auto now = chrono::steady_clock::now();
            for (auto &[name,u] : users) {
                if (u.online && chrono::duration_cast<chrono::seconds>(now - u.last_seen).count() > 10) {
                    cout << "[LOBBY] Player " << name << " timed out, marking offline.\n";
                    u.online = false;
                    save_db();
                }
            }
        }
    }).detach();

    load_db();
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(LOBBY_PORT);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(sock, 10) < 0) { perror("listen"); return 1; }
    cout << "Lobby server listening on port " << LOBBY_PORT << endl;
    while (true) {
        sockaddr_in cli{};
        socklen_t clilen = sizeof(cli);
        int c = accept(sock, (sockaddr*)&cli, &clilen);
        if (c < 0) continue;
        thread t(handle_client, c);
        t.detach();
    }
    close(sock);
    return 0;
}
