#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

void send_bulb_packet(const char *ip, int power, int bright, int warm)
{
    WSADATA wsaData;
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in server;
    char buffer[256];

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        fprintf(stderr, "Winsock Init Failed: %d\n", WSAGetLastError());
        return;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        fprintf(stderr, "Socket error: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(5555);
    if (inet_pton(AF_INET, ip, &server.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid IP address format.\n");
        closesocket(sock);
        WSACleanup();
        return;
    }

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR)
    {
        fprintf(stderr, "Failed to connect to %s: %d\n", ip, WSAGetLastError());
    }
    else
    {
        long long sn = (long long)time(NULL) * 1000;

        int len = snprintf(buffer, sizeof(buffer),
                           "{\"msg\":{\"data\":{\"1\":%d,\"2\":0,\"3\":%d,\"4\":%d,\"5\":65535,\"6\":65535},\"attr\":[1,2,3,4,5,6]},\"pv\":0,\"cmd\":3,\"sn\":\"%lld\",\"res\":0}\n",
                           power, warm, bright, sn);

        if (send(sock, buffer, len, 0) == SOCKET_ERROR)
        {
            fprintf(stderr, "Send failed: %d\n", WSAGetLastError());
        }
        else
        {
            printf("%s -> %s\n", ip, buffer);
           
        }
    }

    Sleep(1000);
    closesocket(sock);
    WSACleanup();
}

void listen_to_bulb(const char *ip)
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(5555);
    inet_pton(AF_INET, ip, &server.sin_addr);

    printf("Attempting to listen to %s on port 5555...\n", ip);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) != SOCKET_ERROR)
    {
        printf("Connected! Listening for bulb status updates (Toggle the app now)...\n");

        char buffer[2048];
        int bytesReceived;

        // Loop to keep the connection open and listen
        while ((bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0)
        {
            buffer[bytesReceived] = '\0';
            printf("\n[BULB DATA]: %s\n", buffer);
        }

        if (bytesReceived == 0)
            printf("Bulb closed the connection.\n");
        else
            printf("Recv failed: %d\n", WSAGetLastError());
    }
    else
    {
        printf("Connect failed: %d\n", WSAGetLastError());
    }

    closesocket(sock);
    WSACleanup();
}
//1 = on/off 0/1
//3 = warmness 0-1000 
//4 = brightness 0-1000
int main()
{
    const char *ip = "192.168.1.106";

    /*
    listen_to_bulb(ip);
    return 0;
    //*/

    //  const char *id = "758713200050c267433a";
    int power = 1;
    int bright = 333;
    int warm = 450;

    if (bright < 1)
    {
        bright = 1;
    }
    else if (bright > 1000)
    {
        bright = 1000;
    }

    if (warm < 1)
    {
        warm = 1;
    }
    else if (warm > 1000)
    {
        warm = 1000;
    }

    send_bulb_packet(ip, power, bright, warm);

    return 0;
}