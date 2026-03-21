// 必须放在所有头文件之前！禁用废弃API警告
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <regex>
#include <chrono>
#include <thread>
#include <stdexcept>

// 跨平台兼容（Windows/Linux通用）
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define MSG_NOSIGNAL 0  // Windows无此宏，置空
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
typedef int SOCKET;
#endif

using namespace std;

// 解析服务器响应数据
unordered_map<string, string> parseServerStatus(const string& udpResponse) {
    unordered_map<string, string> fields = {
        {"hostname", "Unknown Server"},
        {"mapname", "Unknown Map"},
        {"timeleft", "00:00"},
        {"mapstat", ""},
        {"numplayers", "0"},
        {"maxplayers", "0"},
        {"uptime", "0"},
        {"password", "1"},
        {"team_t0", "Unknown"},
        {"team_t1", "Unknown"},
        {"score_t0", "0"},
        {"score_t1", "0"}
    };

    // 正则匹配 "key\\value" 格式
    regex pattern(R"(([a-zA-Z0-9_]+)\\([^\\]+))");
    sregex_iterator it(udpResponse.begin(), udpResponse.end(), pattern);
    sregex_iterator end;
    for (; it != end; ++it) {
        string key = (*it)[1].str();
        string value = (*it)[2].str();
        if (fields.find(key) != fields.end()) {
            fields[key] = value;
        }
    }
    return fields;
}

// 格式化服务器信息输出
string formatServerInfo(const unordered_map<string, string>& fields) {
    // 处理在线时长
    string uptimeStr = "Unknown";
    try {
        int uptimeSec = stoi(fields.at("uptime"));
        int h = uptimeSec / 3600;
        int m = (uptimeSec % 3600) / 60;
        uptimeStr = to_string(h) + "h " + to_string(m) + "m";
    }
    catch (...) {}

    // 处理密码状态
    string passwordStr = (fields.at("password") == "0") ? "No" : "Yes";

    // 清理地图状态字符串
    string mapstatClean = fields.at("mapstat");
    size_t pos = mapstatClean.find("roundlimit_");
    if (pos != string::npos) {
        mapstatClean = mapstatClean.substr(pos + 10);
    }
    // 新增：移除开头/结尾的下划线
    size_t start = mapstatClean.find_first_not_of('_');
    if (start != string::npos) {
        mapstatClean = mapstatClean.substr(start);
    }
    else {
        mapstatClean = "";
    }
    size_t end = mapstatClean.find_last_not_of('_');
    if (end != string::npos) {
        mapstatClean = mapstatClean.substr(0, end + 1);
    }
    else {
        mapstatClean = "";
    }

    // 拼接输出内容
    string output = "\nServer Info:\n";
    output += "-------------------------------------------------\n";
    output += "Server Name: " + fields.at("hostname") + "\n";
    output += "Server Map: " + fields.at("mapname") + "\n";
    output += "Server Map Time: " + fields.at("timeleft") + "\n";
    output += "Server Map Stats: " + mapstatClean + "\n";
    output += "Players: " + fields.at("numplayers") + "/" + fields.at("maxplayers") + "\n";
    output += "Server Uptime: " + uptimeStr + "\n";
    output += "Server Password: " + passwordStr + "\n";
    output += "Team 0 (IGI): Score = " + fields.at("score_t0") + "\n";
    output += "Team 1 (CON): Score = " + fields.at("score_t1") + "\n";
    output += "-------------------------------------------------\n";

    // 玩家信息
    if (fields.at("numplayers") == "0") {
        output += "Player Info: No players online\n";
    }
    else {
        output += "Player Info: " + fields.at("numplayers") + " players online (details not provided)\n";
    }
    return output;
}

// 发送UDP请求并接收响应（核心函数，已修复所有问题）
string sendUdpStatusRequest(const string& serverIp, int serverPort, int timeout = 5) {
    // 1. Windows初始化Winsock
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return "Error: Winsock initialization failed (WSAStartup)";
    }
#endif

    // 2. 创建UDP套接字
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        string err = "Error: Create socket failed (errno: ";
#ifdef _WIN32
        err += to_string(WSAGetLastError());
#else
        err += to_string(errno);
#endif
        err += ")";
#ifdef _WIN32
        WSACleanup();
#endif
        return err;
    }

    // 3. 配置套接字（端口复用+增大接收缓冲区）
    int optVal = 1;
    // 允许端口复用，避免端口占用
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&optVal, sizeof(optVal)) == SOCKET_ERROR) {
        cerr << "Warning: Set SO_REUSEADDR failed (non-fatal)" << endl;
    }
    // 增大接收缓冲区，避免响应截断
    int recvBufSize = 16384;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&recvBufSize, sizeof(recvBufSize)) == SOCKET_ERROR) {
        cerr << "Warning: Set SO_RCVBUF failed (non-fatal)" << endl;
    }

    // 4. 设置接收超时
    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == SOCKET_ERROR) {
        closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return "Error: Set socket timeout failed";
    }

    // 5. 构造服务器地址（兼容老旧IP解析）
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);
    serverAddr.sin_addr.s_addr = inet_addr(serverIp.c_str());
    if (serverAddr.sin_addr.s_addr == INADDR_NONE) {
        closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return "Error: Invalid server IP address";
    }

    // 6. 构造请求数据（和Python一致：\status\）
    const char* requestData = "\\status\\";
    int requestLen = strlen(requestData);

    try {
        // 7. 多次发送请求（重试2次，模拟Python行为）
        for (int retry = 0; retry < 2; retry++) {
            int sendRet = sendto(
                sock,
                requestData,
                requestLen,
                MSG_NOSIGNAL,
                (sockaddr*)&serverAddr,
                sizeof(serverAddr)
            );
            if (sendRet == SOCKET_ERROR) {
                throw runtime_error("Send UDP packet failed (retry " + to_string(retry) + ")");
            }
            this_thread::sleep_for(chrono::milliseconds(100)); // 发送间隔
        }

        cout << "已向 " << serverIp << ":" << serverPort << " 发送状态请求（2次重试）" << endl;

        // 8. 接收服务器响应
        char recvBuf[16384] = { 0 };
        sockaddr_in fromAddr{};
        socklen_t fromAddrLen = sizeof(fromAddr);
        int recvLen = recvfrom(
            sock,
            recvBuf,
            sizeof(recvBuf) - 1,
            0,
            (sockaddr*)&fromAddr,
            &fromAddrLen
        );

        // 处理接收错误（含超时）
        if (recvLen == SOCKET_ERROR) {
#ifdef _WIN32
            int errCode = WSAGetLastError();
            if (errCode == WSAETIMEDOUT) {
                throw runtime_error("Receive timeout (WSAETIMEDOUT)");
            }
            else {
                throw runtime_error("Receive failed (WSAError: " + to_string(errCode) + ")");
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw runtime_error("Receive timeout (EAGAIN/EWOULDBLOCK)");
            }
            else {
                throw runtime_error("Receive failed (errno: " + to_string(errno) + ")");
            }
#endif
        }

        // 打印响应来源信息
        char fromIp[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &fromAddr.sin_addr, fromIp, sizeof(fromIp));
        cout << "已收到来自 " << fromIp << ":" << ntohs(fromAddr.sin_port)
            << " 的响应，长度：" << recvLen << " 字节" << endl;

        // 9. 解析并格式化响应数据
        string responseStr(recvBuf, recvLen);
        auto fields = parseServerStatus(responseStr);
        string result = formatServerInfo(fields);

        // 10. 清理资源
        closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif

        return result;

    }
    catch (const exception& e) {
        // 异常处理+资源清理
        closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return "Error: " + string(e.what());
    }
}

// 主函数（修改这里的IP/端口即可查询不同服务器）
int main() {
    // 服务器IP和端口（可直接修改）
    string SERVER_IP;
    int SERVER_PORT;
    cout << "请输入IGI2服务器IP地址:";
    cin >> SERVER_IP;
    cout << "请输入IGI2服务器端口:";
    cin >> SERVER_PORT;
    // 发送请求并获取结果
    string result = sendUdpStatusRequest(SERVER_IP, SERVER_PORT);

    // 打印最终结果
    cout << "\n==================================================" << endl;
    cout << "解析后的服务器状态：" << endl;
    cout << result;
    cout << "==================================================" << endl;
    system("pause");
    return 0;
}
