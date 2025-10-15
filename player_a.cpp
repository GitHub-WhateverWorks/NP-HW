#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

//const string LOBBY_HOST = "127.0.0.1";
const string LOBBY_HOST = "140.113.17.14";

const int LOBBY_PORT = 12000;
const int UDP_PORT_MIN = 17000;
const int UDP_PORT_MAX = 17010;

bool send_udp_json(int sock, const sockaddr_in &to, const json &j) {
    string s = j.dump();
    ssize_t n = sendto(sock, s.c_str(), s.size(), 0, (sockaddr*)&to, sizeof(to));
    return n == (ssize_t)s.size();
}

bool recv_udp_json(int sock, json &out, sockaddr_in &from, int timeout_ms) {
    fd_set set; FD_ZERO(&set); FD_SET(sock,&set);
    timeval tv; tv.tv_sec = timeout_ms/1000; tv.tv_usec = (timeout_ms%1000)*1000;
    int rc = select(sock+1, &set, nullptr, nullptr, &tv);
    if (rc <= 0) return false;
    char buf[4096]; sockaddr_in src{}; socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(sock, buf, sizeof(buf)-1, 0, (sockaddr*)&src, &srclen);
    if (n <= 0) return false;
    buf[n] = 0;
    out = json::parse(string(buf));
    from = src;
    return true;
}

bool send_tcp_json(int fd, const json &j) {
    string s = j.dump(); s.push_back('\n');
    ssize_t total=0; const char* data = s.c_str(); ssize_t tosend = s.size();
    while (total < tosend) {
        ssize_t n = send(fd, data+total, tosend-total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool recv_tcp_line(int fd, string &out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n==0) {
            cout<<"[ERROR] Connection closed by peer\n";
        }
        if (n < 0){
            cout<<"[ERROR] recv() failed\n";
            return false;
        }
        if (c == '\n') break;
        out.push_back(c);
    }
    return true;
}

json lobby_request(const json &req) {
    //cout << "[DEBUG] Creating socket...\n";
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("[DEBUG] socket failed"); return json(); }
    sockaddr_in addr{}; 
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(LOBBY_PORT);
    inet_pton(AF_INET, LOBBY_HOST.c_str(), &addr.sin_addr);
    //cout << "[DEBUG] Connecting to lobby " << LOBBY_HOST << ":" << LOBBY_PORT << "...\n";
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("[DEBUG] connect failed"); close(s); return json(); }
    //cout << "[DEBUG] Connected successfully.\n";
    string out = req.dump(); out.push_back('\n'); 
    //cout << "[DEBUG] Sending JSON request: " << out;
    ssize_t sent = send(s, out.c_str(), out.size(), 0);
    if (sent != (ssize_t)out.size()) {
        perror("[DEBUG] send failed");
        close(s);
        return json();
    }
    //cout << "[DEBUG] Sent " << sent << " bytes.\n";
    string response;
    char buf[1024];
     //cout << "[DEBUG] Receiving response...\n";
    while (true) {
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        //cout << "[DEBUG] recv returned: " << n << endl;
        if (n <= 0) break;               // disconnected or error
        response.append(buf, n);
        //cout << "[DEBUG] Current response buffer: " << response << endl;
        if (response.find('\n') != string::npos){
            //cout << "[DEBUG] Newline found in response.\n";
            break;
        }
    }
    close(s);

    size_t pos = response.find('\n');
    if (pos != string::npos) response = response.substr(0, pos);

    //cout << "[DEBUG] Server replied: " << response << endl;  

    try { return json::parse(response); }
    catch (...) { return json(); }
}



void print_board(const vector<string>& b) {
    cout << "\nBoard:\n";
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c) {
            cout << (b[3*r+c].empty()? "." : b[3*r+c]) << (c<2? " " : "");
        }
        cout << "\n";
    }
}

int check_tictactoe(const vector<string>& b) {
    vector<array<int,3>> lines = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for (auto &ln : lines) {
        string a=b[ln[0]], c=b[ln[1]], d=b[ln[2]];
        if (!a.empty() && a==c && c==d) return (a=="X"? 1 : 2);
    }
    if (all_of(b.begin(), b.end(), [](const string &s){ return !s.empty(); })) return 3;
    return 0;
}

int main(){
    cout << "Welcome to the game!\n";

    string username, password;


    string choice;
    while(true){
        while (true) {
            cout << "Do you want to (L)ogin or (R)egister? ";
            getline(cin, choice);
            if (choice == "L" || choice == "l" || choice == "R" || choice == "r") break;
        }
        cout << "PlayerA username: ";
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
        //Periodic Status update
        atomic<bool> still_online(true);

        thread status_thread([&](){
            while (still_online) {
                this_thread::sleep_for(chrono::seconds(5));  
                json resp = lobby_request({
                    {"cmd", "STATUS"},
                    {"username", username},
                    {"extra", {{"xp", 1}}} 
                });
                //cout << "[DEBUG] Lobby status tick: " << resp.dump() << "\n";
            }
        });

        // UDP socket for sending invites
        int udp = socket(AF_INET,  c, 0);
        if (udp < 0) { perror("udp"); return 1; }

        vector<pair<string,int>> servers;
        vector<string> linux_ips = {"140.113.17.11","140.113.17.12","140.113.17.13","140.113.17.14"};
        for(int i=0;i<linux_ips.size();i++){
            //for (int p=UDP_PORT_MIN;p<=UDP_PORT_MAX;++p) servers.push_back({"127.0.0.1", p});
            for (int p=UDP_PORT_MIN;p<=UDP_PORT_MAX;++p) servers.push_back({linux_ips[i], p});
        }
        sockaddr_in target;
        bool Found=false;
        while(!Found){
            cout<<"[DEBUG] Start probing\n";
            srand(time(nullptr));
            int nonce = rand() % 1000000;
            vector<pair<sockaddr_in, json>> candidates;
            for (auto &sv : servers) {
                sockaddr_in to{}; 
                to.sin_family = AF_INET; 
                to.sin_port = htons(sv.second);
                inet_pton(AF_INET, sv.first.c_str(), &to.sin_addr);
                json check = {{"type","CHECK"},{"from",username},{"nonce",nonce}};
                send_udp_json(udp, to, check);
                cout<<"[DEBUG] Sending Checks\n";
                json reply; sockaddr_in from;
                if (recv_udp_json(udp, reply, from, 500)) {  // short timeout
                    if (reply.value("type","") == "CHECK_RESPONSE" && reply.value("nonce",0) == nonce) {
                        if (reply.value("status","") == "ONLINE") {
                            candidates.push_back({from, reply});
                            cout << "Found PlayerB at " << inet_ntoa(from.sin_addr) << ":" << ntohs(from.sin_port) << "\n";
                        }
                    }
                }
            }

            if (candidates.empty()) {
                cout << "No available PlayerB found\n";
                continue;
            }

            // Step 2: Let the user choose a PlayerB
            cout << "\nAvailable PlayerB(s):\n";
            for (size_t i = 0; i < candidates.size(); ++i) {
                sockaddr_in &from = candidates[i].first;
                cout << i+1 << ": " << inet_ntoa(from.sin_addr) << ":" << ntohs(from.sin_port) << "\n";
            }

            size_t choice_p = 0;
            while (true) {
                cout << "Enter the number of the PlayerB to invite: ";
                string s; 
                getline(cin, s);
                try { 
                    int idx = stoi(s); 
                    if (idx>=1 && idx-1 < candidates.size()){ choice_p = idx-1; break;}
                } catch(...) { continue; }
                
            }

            target = candidates[choice_p].first;

            // Step 3: Send the actual INVITE to the selected PlayerB
            json invite = {{"type","INVITE"},{"from",username},{"nonce",nonce}};
            send_udp_json(udp, target, invite);
            cout << "Invite sent to " << inet_ntoa(target.sin_addr) << ":" << ntohs(target.sin_port) << "\n";
            json reply;
            sockaddr_in from{};
            if (!recv_udp_json(udp, reply, from, 10000)) {
                cout << "[WARN] No reply (timeout)\n";
                continue;
            }
            string rtype = reply.value("type","");
            string response = reply.value("response","");
            if (rtype != "INVITE_RESPONSE" || response != "ACCEPT") {
                cout << "[INFO] Invite declined.\n";
            }else{
                cout<<"[INFO] Invite Accepted!\n";
                Found=true;
            }
        }
        // Start TCP server
        int tcps = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1; setsockopt(tcps, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
        int tcp_port = 20000;
        while (true) {
            addr.sin_port = htons(tcp_port);
            if (bind(tcps, (sockaddr*)&addr, sizeof(addr))==0) break;
            ++tcp_port;
        }
        if (listen(tcps, 1) < 0) {
            perror("listen failed");
            return 0;
        }
        

        // send CONNECT_INFO to B via UDP (we must send A's reachable IP)
        char hostname[256]; 
        gethostname(hostname, sizeof(hostname));
        // get first non-loopback IP (simple)
        string myip = "140.113.17.12";
        json info = {{"type","CONNECT_INFO"},{"ip",myip},{"port",tcp_port}};
        send_udp_json(udp, target, info);
        cout << "Sent CONNECT_INFO to " << inet_ntoa(target.sin_addr) << ":"<<ntohs(target.sin_port) << " (tcp port " << tcp_port << ")\n";

        // accept TCP
        sockaddr_in peer{}; 
        socklen_t plen = sizeof(peer);
        cout << "[DEBUG] Waiting for TCP connection...\n";
        int conn = accept(tcps, (sockaddr*)&peer, &plen);
        if (conn < 0) { perror("accept"); return 1; }
        cout << "PlayerB connected via TCP\n";
        // Game
        vector<string> board(9, "");
        string line;
        auto send_tcp = [&](const json &j){ string s = j.dump(); s.push_back('\n'); send(conn, s.c_str(), s.size(), 0); };
        auto recv_tcp = [&](string &out)->bool{
            out.clear(); char c;
            while (true) {
                ssize_t n = recv(conn, &c, 1, 0);
                if (n <= 0) return false;
                if (c=='\n') break;
                out.push_back(c);
            }
            return true;
        };
        send_tcp(json{{"type","GAME_START"},{"you","O"},{"opponent",username},{"board",board},{"first_turn","X"}});
        string turn = "X";
        //Periodic check Alive
        atomic<bool> opponent_online(true);
        atomic<bool> running(true);
        string opponent_name = username; // get this from handshake or game start
        thread online_checker([&]() {
            while (running) {
                json status_req = {{"cmd", "ONLINE_STATUS"}, {"username", opponent_name}};
                json status_resp = lobby_request(status_req);

                bool on = status_resp.value("online", true);
                if (!on) {
                    cout << "\n[INFO] Opponent " << opponent_name << " went offline.\n";
                    opponent_online = false;
                    running = false;
                    break;
                }

                this_thread::sleep_for(chrono::seconds(5));
            }
        });

        while (running) {
            if (!opponent_online) {
                cout << "[INFO] Game stopped — opponent is offline.\n";
                break;
            }
            if (turn == "X") {
                print_board(board);
                int pos = -1;
                while (true) {
                    cout << "Your move (0-8): ";
                    string s; getline(cin, s);
                    try { pos = stoi(s); } catch(...) { continue; }
                    if (pos>=0 && pos<9 && board[pos].empty()) break;
                }
                board[pos] = "X";
                //send_tcp(json{{"type","MOVE"},{"pos",pos}});
                cout<<"[INFO] Waiting for opponent to move...\n";
            } else {
                send_tcp(json{{"type","MOVE_REQ"},{"board",board}});
                if (!recv_tcp(line)) { 
                    cout << "[ERROR] Peer disconnected\n"; 
                    break; 
                }
                try { json m = json::parse(line); if (m.value("type","")=="MOVE") { int p = m.value("pos",-1); if (p>=0 && p<9) board[p] = "O"; } } catch(...) {}
            }
            int res = check_tictactoe(board);
            if (res != 0) {
                if (res == 1){
                    send_tcp(json{{"type","GAME_END"},{"result","LOSE"},{"board",board}});
                    cout<<"You Won!\n";
                } else if (res == 2){
                    send_tcp(json{{"type","GAME_END"},{"result","WIN"},{"board",board}});
                    cout<<"You Lost...\n";
                } else{
                    send_tcp(json{{"type","GAME_END"},{"result","DRAW"},{"board",board}});
                    cout<<"Draw!\n";
                }
                break;
            }
            turn = (turn=="X")? "O" : "X";
        }
        running=false;
        still_online=false;
        status_thread.join();
        online_checker.join();
        close(conn);
        close(tcps);
        close(udp);
    }
    return 0;
}
