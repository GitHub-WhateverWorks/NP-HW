#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <atomic>
#include <fcntl.h>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

const string LOBBY_HOST = "140.113.17.14";
const int LOBBY_PORT = 12000;
const int UDP_MIN = 17000;
const int UDP_MAX = 17010;

bool send_udp_json(int sock, const sockaddr_in &to, const json &j) {
    string s = j.dump();
    ssize_t n = sendto(sock, s.c_str(), s.size(), 0, (sockaddr*)&to, sizeof(to));
    return n == (ssize_t)s.size();
}

bool recv_udp_json(int sock, json &out, sockaddr_in &from, int timeout_ms=0) {
    fd_set set; FD_ZERO(&set); FD_SET(sock,&set);
    timeval tv; tv.tv_sec = timeout_ms/1000; tv.tv_usec = (timeout_ms%1000)*1000;
    int rc = select(sock+1, &set, nullptr, nullptr, (timeout_ms>0?&tv:nullptr));
    if (rc <= 0) return false;
    char buf[4096]; sockaddr_in src{}; socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(sock, buf, sizeof(buf)-1, 0, (sockaddr*)&src, &srclen);
    if (n <= 0) return false;
    buf[n] = 0;
    try { out = json::parse(string(buf)); from = src; return true; } catch (...) { return false; }
}

bool send_tcp_json(int fd, const json &j) {
    string s = j.dump(); s.push_back('\n');
    ssize_t total=0; const char* buf = s.c_str(); ssize_t tosend = s.size();
    while (total < tosend) {
        ssize_t n = send(fd, buf+total, tosend-total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool recv_tcp_line(int fd, string &out) {
    out.clear();
    char c;
    //cout<<"[DEBUG] Receiving TCP...\n";
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') break;
        out.push_back(c);
    }
    //cout<<"[DEBUG] Receive TCP complete.\n"<<out<<"\n";
    return true;
}

json lobby_request(const json &req) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return json(); }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(LOBBY_PORT);
    inet_pton(AF_INET, LOBBY_HOST.c_str(), &addr.sin_addr);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) { close(s); return json(); }
    send_tcp_json(s, req);
    string line;
    if (!recv_tcp_line(s, line)) { close(s); return json(); }
    close(s);
    try { return json::parse(line); } catch (...) { return json(); }
}



void print_board(const vector<string>& b) {
    cout << "\nBoard:\n";
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c) {
            string v = b[3*r+c]; cout << (v.empty()? "." : v) << (c<2? " " : "");
        }
        cout << "\n";
    }
}

int main(){
    cout << "Welcome to the game!\n";
    //Login & Register
    string username, password;
    string choice;
    while(true){
        while (true) {
            cout << "Do you want to (L)ogin or (R)egister? ";
            getline(cin, choice);
            if (choice == "L" || choice == "l" || choice == "R" || choice == "r") break;
        }
        cout << "PlayerB username: ";
        getline(cin, username);
        cout << "Password: ";
        getline(cin, password);
        json resp;
        if (choice == "R" || choice == "r") {

            cout << "Registering...\n";
            resp = lobby_request({{"cmd","REGISTER"},{"username",username},{"password",password}});
            cout << "Register response: " << resp.dump() << "\n";
            if (resp.value("status","ERR") != "OK") {
                cout << "Registration failed. Exiting.\n";
                continue;
            }
        }

        cout << "Logging in...\n";
        resp = lobby_request({{"cmd","LOGIN"},{"username",username},{"password",password}});
        cout << "Login response: " << resp.dump() << "\n";
        if (resp.value("status","ERR") != "OK") {
            cout << "Login failed. Exiting.\n";
            continue;
        }

        cout << "Welcome, " << username << "!\n";
        break;
    }
    
    while(true){ //return to lobby
        // UDP
        int udp = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp < 0) { perror("udp socket"); return 1; }
        sockaddr_in myaddr{}; myaddr.sin_family = AF_INET; myaddr.sin_addr.s_addr = INADDR_ANY;
        int bound_port = -1;
        for (int p = UDP_MIN; p <= UDP_MAX; ++p) {
            myaddr.sin_port = htons(p);
            if (bind(udp, (sockaddr*)&myaddr, sizeof(myaddr))==0) { bound_port = p; break; }
        }
        if (bound_port == -1) { cerr << "No free UDP port\n"; return 1; }
        cout << "Listening for invites on UDP port " << bound_port << endl;

        // Lobby status updater
        atomic<bool> running(true);
        thread status_thread([&](){
            while (running) {
                this_thread::sleep_for(chrono::seconds(5));
                lobby_request({{"cmd","STATUS"},{"username",username},{"extra", {{"xp",1}}}});
                //cout << "[DEBUG] Lobby status update tick\n";
            }
        });

        //Invite, TCP, Game
        bool play = true;
        while (true) {
            json msg; 
            sockaddr_in from{};
            recv_udp_json(udp, msg, from, 0);
            string msgtype = msg.value("type","");
            cout<<"[DEBUG] msg: "<<msg.dump()<<"\n";
            //Probe response
            if (msgtype == "CHECK"){
                cout<<"[DEBUG] Received cmd CHECK\n";
                json reply = {{"type","CHECK_RESPONSE"},{"status","ONLINE"},{"nonce",msg.value("nonce",0)}};
                send_udp_json(udp, from, reply);
                continue;
            }else if (msgtype == "INVITE") {
                int nonce = msg.value("nonce",0);
                char buf[INET_ADDRSTRLEN]; 
                inet_ntop(AF_INET, &from.sin_addr, buf, sizeof(buf));
                cout << "Invitation from " << buf << ":" << ntohs(from.sin_port) << " nonce="<<nonce<<"\n";
                cout << "Accept invite? (y/n): ";
                string ans; 
                getline(cin, ans);
                json reply;
                if (!ans.empty() && (ans[0]=='y' || ans[0]=='Y')) {
                    reply = {{"type","INVITE_RESPONSE"},{"response","ACCEPT"},{"nonce",nonce}};
                    send_udp_json(udp, from, reply);
                    cout << "Accepted. Waiting for CONNECT_INFO...\n";
                    //TCP
                    json info; 
                    sockaddr_in inf_from{};
                    bool got = recv_udp_json(udp, info, inf_from, 10000); 
                    if (!got || info.value("type","") != "CONNECT_INFO") {
                        cout << "No CONNECT_INFO received.\n";
                        continue;
                    }
                    string aip = info.value("ip","");
                    int aport = info.value("port",0);
                    cout << "Connecting to A " << aip << ":"<<aport<<" via TCP...\n";
                    
                    int conn = socket(AF_INET, SOCK_STREAM, 0);
                    sockaddr_in aaddr{}; aaddr.sin_family = AF_INET; aaddr.sin_port = htons(aport);
                    inet_pton(AF_INET, aip.c_str(), &aaddr.sin_addr);
                    if (connect(conn, (sockaddr*)&aaddr, sizeof(aaddr)) < 0) {
                        perror("connect");
                        close(conn);
                        continue;
                    }

                    //Game
                    auto send_tcp = [&](const json& j){ string out = j.dump(); out.push_back('\n'); send(conn, out.c_str(), out.size(), 0); };
                    auto recv_tcp = [&](string &line)->bool{
                        line.clear(); char c;
                        while (true) {
                            ssize_t n = recv(conn, &c, 1, 0);
                            if (n <= 0) return false;
                            if (c=='\n') break;
                            line.push_back(c);
                        }
                        return true;
                    };

                    
                    while (play) {
                        string line;
                        if (!recv_tcp(line)) { cout << "Disconnected from A\n"; break; }
                        json m;
                        try { m = json::parse(line); } catch(...) { continue; }
                        string t = m.value("type","");
                        if (t == "GAME_START") {
                            cout << "[INFO] Game started\n";
                        } else if (t == "MOVE_REQ") {
                            vector<string> board = m.value("board", vector<string>(9,""));
                            print_board(board);
                            int pos = -1;
                            while (true) {
                                cout << "Your move (0-8): ";
                                string sline; getline(cin, sline);
                                if (sline.empty()) continue;
                                try { pos = stoi(sline); } catch(...) { continue; }
                                if (pos>=0 && pos<9 && board[pos].empty()) break;
                            }
                            send_tcp(json{{"type","MOVE"},{"pos",pos}}); 
                            cout<<"[INFO] Waiting for opponent to move...\n";
                        } else if (t == "GAME_END") {
                            string result = m.value("result","");
                            cout << "Game ended: You " 
                                << ((result=="WIN")?"Won!":
                                    (result=="LOSE")?"Lost...":"Draw!") 
                                << "\n";
                            play = false;
                            break;
                        } else {
                            cout << "TCP msg: " << m.dump() << "\n";
                            cout <<"[DEBUG] ???\n";
                        }
                    }
                    cout<<"[DEBUG] Return to lobby\n";
                    close(conn);
                } else {
                    reply = {{"type","INVITE_RESPONSE"},{"response","DECLINE"},{"nonce",nonce}};
                    send_udp_json(udp, from, reply);
                    cout << "Declined.\n";
                }
                cout<<"[DEBUG] Invite loop complete\n";
            } else {
                cout<<"[DEBUG] Exit\n";
                break;
            }
            cout<<"[DEBUG] Largest while loop\n";
            if(!play) break;
        }
        cout<<"[DEBUG] Final shut down\n";
        running = false;
        status_thread.join();
        
        close(udp);
    }
    return 0;
}
