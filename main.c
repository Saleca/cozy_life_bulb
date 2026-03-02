#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mosquitto.h>
#include "cjson/cJSON.h"

// --- Configuration ---
#define TARGET_PORT 5555
#define MQTT_BROKER_IP "192.168.1.120"
#define SUBNET_PREFIX "192.168.1."
#define SCAN_TIMEOUT_MS 500

#define BULB_PAYLOAD "{\"msg\":{\"data\":{\"1\":%d,\"2\":0,\"3\":%d,\"4\":%d,\"5\":65535,\"6\":65535},\"attr\":[1,2,3,4,5,6]},\"pv\":0,\"cmd\":3,\"sn\":\"%lld\",\"res\":0}\n"
#define BULB_PAYLOAD_POWER "{\"msg\":{\"data\":{\"1\":%d},\"attr\":[1]},\"pv\":0,\"cmd\":3,\"sn\":\"%lld\",\"res\":0}\n"
#define BULB_PAYLOAD_BRIGHTNESS "{\"msg\":{\"data\":{\"1\":%d,\"4\":%d},\"attr\":[1,4]},\"pv\":0,\"cmd\":3,\"sn\":\"%lld\",\"res\":0}\n"
#define BULB_PAYLOAD_WARMTH "{\"msg\":{\"data\":{\"1\":%d,\"2\":0,\"3\":%d,\"5\":65535,\"6\":65535},\"attr\":[1,2,3,5,6]},\"pv\":0,\"cmd\":3,\"sn\":\"%lld\",\"res\":0}\n"

#define HA_PAYLOAD "{\"state\":\"%s\",\"brightness\":%d,\"color_mode\": \"color_temp\",\"color_temp\":%d}"
#define HA_PAYLOAD_POWER "{\"state\":\"%s\"}"
#define HA_PAYLOAD_BRIGHTNESS "{\"state\":\"%s\",\"brightness\":%d,\"color_mode\": \"color_temp\"}"
#define HA_PAYLOAD_WARMTH "{\"state\":\"%s\",\"color_mode\": \"color_temp\",\"color_temp\":%d}"

#define DEVICE_NAME_PREFIX "cozy_light_"
#define DEVICE_NAME DEVICE_NAME_PREFIX "%d"
#define CONFIG_TOPIC "homeassistant/light/" DEVICE_NAME "/config"
#define TOPIC_ADDRESS "home/" DEVICE_NAME
#define STATE_TOPIC TOPIC_ADDRESS "/state"
#define SET_TOPIC TOPIC_ADDRESS "/set"
#define AVAILABILITY_TOPIC TOPIC_ADDRESS "/availability"
// --- Global State ---
int device_sockets[16];
char device_ips[16][16];
int device_count = 0;

struct mosquitto *mosq = NULL;

// --- Function Prototypes ---
SOCKET open_tcp_socket(const char *ip);
void close_tcp_socket(SOCKET current_socket);

int probe_ip(const char *ip);
void scan_for_bulbs();
void send_discovery_to_ha(struct mosquitto *mosq, int id);
void bulb_command_message(struct mosquitto *mosq, const int id, const char *payload);
void send_bulb_packet(const int id, int power, int brightness, int warm);
void send_bulb_packet_power(const int id, int power);
void send_bulb_packet_brightness(const int id, int brightness);
void send_bulb_packet_warmth(const int id, int warm);
void ha_state_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);

SOCKET open_tcp_socket(const char *ip)
{
    SOCKET current_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (current_socket == INVALID_SOCKET)
    {
        fprintf(stderr, "Socket error: %d\n", WSAGetLastError());
        WSACleanup();
        return INVALID_SOCKET;
    }

    unsigned long mode = 1;
    ioctlsocket(current_socket, FIONBIO, &mode);

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(TARGET_PORT);

    if (inet_pton(AF_INET, ip, &server.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid IP address format.\n");
        closesocket(current_socket);
        return INVALID_SOCKET;
    }

    if (connect(current_socket, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR)
    {
        // fprintf(stderr, "Failed to connect to %s: %d\n", ip, WSAGetLastError());
    }

    return current_socket;
}

void close_tcp_socket(SOCKET current_socket)
{
    closesocket(current_socket);
}

int probe_ip(const char *ip)
{
    SOCKET current_socket = open_tcp_socket(ip);

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(current_socket, &write_set);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = SCAN_TIMEOUT_MS * 1000;

    int result = (select(0, NULL, &write_set, NULL, &tv) > 0);

    if (result)
    {
        strcpy(device_ips[device_count], ip);
        device_sockets[device_count] = current_socket;
        device_count++;
        printf("Found light at: %s\n", ip);
    }
    else
    {
        close_tcp_socket(current_socket);
    }

    return result;
}

void scan_for_bulbs()
{
    printf("Scanning subnet %s0/24...\n", SUBNET_PREFIX);
    for (int i = 104; i < 255; i++) // i=1
    {
        char current_test_ip[16];
        sprintf(current_test_ip, "%s%d", SUBNET_PREFIX, i);
        printf("testing: %s\n", current_test_ip);

        if (probe_ip(current_test_ip))
        {
            // remove for extra lamps
            return;
        }
    }
}

// Formula: output = ((input - in_min) * (out_max - out_min) / (in_max - in_min)) + out_min
uint16_t convert_to_kelvin(uint16_t input)
{
    if (input == 0)
    {
        return 2000;
    }
    else if (input >= 1000)
    {
        return 6535;
    }

    // ((input - 0) * (6535 - 2000) / (1000 - 0)) + 2000
    return (uint16_t)(((uint32_t)input * 4535) / 1000) + 2000;
}
uint16_t convert_to_permille(uint16_t input)
{
    if (input <= 2000)
    {
        return 0;
    }
    else if (input >= 6535)
    {
        return 1000;
    }

    // ((input - 2000) * 1000 - 0) / (6535 - 2000)
    return (uint16_t)(((uint32_t)(input - 2000) * 1000) / 4535);
}

void get_device_name(char *dest, int id)
{
    sprintf(dest, DEVICE_NAME, id);
}

int get_device_id(const char *topic)
{
    const char *ptr = strstr(topic, DEVICE_NAME_PREFIX);

    if (!ptr)
    {
        return -1;
    }

    ptr += strlen(DEVICE_NAME_PREFIX);
    if (!isdigit((unsigned char)*ptr))
    {
        return -1;
    }

    return atoi(ptr);
}

void send_discovery_to_ha(struct mosquitto *mosq, int id)
{
    char name[32];
    get_device_name(name, id);

    char config_topic[64];
    sprintf(config_topic, CONFIG_TOPIC, id);

    char availability_topic[64];
    sprintf(availability_topic, AVAILABILITY_TOPIC, id);

    char set_topic[64];
    sprintf(set_topic, SET_TOPIC, id);

    char state_topic[64];
    sprintf(state_topic, STATE_TOPIC, id);

    cJSON *config_payload_cjson = cJSON_CreateObject();
    cJSON_AddStringToObject(config_payload_cjson, "schema", "json");
    cJSON_AddStringToObject(config_payload_cjson, "name", "Cozy white light");
    cJSON_AddStringToObject(config_payload_cjson, "unique_id", name);
    cJSON_AddStringToObject(config_payload_cjson, "command_topic", set_topic);
    cJSON_AddStringToObject(config_payload_cjson, "state_topic", state_topic);
    cJSON_AddBoolToObject(config_payload_cjson, "brightness", true);
    cJSON_AddNumberToObject(config_payload_cjson, "brightness_scale", 1000);
    const char *color_modes_array[] = {"color_temp"};
    cJSON_AddItemToObject(config_payload_cjson, "supported_color_modes", cJSON_CreateStringArray(color_modes_array, 1));
    cJSON_AddBoolToObject(config_payload_cjson, "color_temp_kelvin", true);
    cJSON_AddStringToObject(config_payload_cjson, "availability_topic", availability_topic);
    cJSON_AddStringToObject(config_payload_cjson, "payload_available", "online");
    cJSON_AddStringToObject(config_payload_cjson, "payload_not_available", "offline");
    /*
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    const char *ids[] = { name };
    cJSON_AddItemToObject(device, "identifiers", cJSON_CreateStringArray(ids, 1));
    cJSON_AddStringToObject(device, "name", "cozy light 2");
    cJSON_AddStringToObject(device, "manufacturer", "Saleca");
    */

    char *config_payload = cJSON_PrintUnformatted(config_payload_cjson);
    if (config_payload)
    {
        mosquitto_subscribe(mosq, NULL, set_topic, 0);

        mosquitto_publish(mosq, NULL, config_topic, (int)strlen(config_payload), config_payload, 0, true);
        printf("published config: %s\n", config_payload);

        mosquitto_publish(mosq, NULL, availability_topic, 6, "online", 0, true);

        cJSON *initial_state_cjson = cJSON_CreateObject();
        cJSON_AddStringToObject(initial_state_cjson, "state", "ON");
        cJSON_AddNumberToObject(initial_state_cjson, "brightness", 500);
        cJSON_AddNumberToObject(initial_state_cjson, "color_temp", convert_to_kelvin(500));
        char *initial_state = cJSON_PrintUnformatted(initial_state_cjson);
        if (initial_state)
        {
            mosquitto_publish(mosq, NULL, state_topic, (int)strlen(initial_state), initial_state, 0, true);
            printf("published state: %s\n", initial_state);
            free(initial_state);
        }
        cJSON_Delete(initial_state_cjson);

        free(config_payload);
    }
    cJSON_Delete(config_payload_cjson);
}

// received message from HA
void ha_state_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    if (!msg->payload)
    {
        return;
    }

    int device_id = get_device_id(msg->topic);

    if (device_id < 0)
    {
        printf("failled to get device id: %s\n", msg->topic);
        return;
    }
    else if (device_id >= device_count)
    {
        printf("device id not registered: %s\n", msg->topic);
        return;
    }

    char *payload = (char *)msg->payload;
    printf("[HA] message: %s\n", payload);

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            printf("[cjson] error: %s\n", error_ptr);
        }
        return;
    }

    int power = -1;
    int brightness = -1;
    int warmth = -1;

    cJSON *warm = cJSON_GetObjectItemCaseSensitive(root, "color_temp");
    if (cJSON_IsNumber(warm))
    {
        warmth = convert_to_permille(warm->valueint);
        // power = 1;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state) && (state->valuestring != NULL))
    {
        if (strcmp(state->valuestring, "OFF") == 0)
        {
            power = 0;
        }
        else // if (strcmp(state->valuestring, "ON") == 0)
        {
            power = 1;
        }
    }

    cJSON *bright = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(bright))
    {
        brightness = bright->valueint;
        if (brightness == 0)
        {
            power = 0;
        }
        else
        {
            // power = 1;
        }
    }

    cJSON_Delete(root);

    if (power >= 0 && brightness < 0 && warmth < 0)
    {
        send_bulb_packet_power(device_id, power);
    }
    else if (brightness > 0 && warmth < 0)
    {
        send_bulb_packet_brightness(device_id, brightness);
    }
    else if (brightness < 0 && warmth > 0)
    {
        send_bulb_packet_warmth(device_id, warmth);
    }
    else
    {
        send_bulb_packet(device_id, power, brightness, warmth);
    }
}

// send message to HA
void bulb_command_message(struct mosquitto *mosq, const int id, const char *payload)
{
    printf("[BULB] message: %s\n", payload);

    cJSON *cjson_root = cJSON_Parse(payload);
    cJSON *cjson_command = cJSON_GetObjectItemCaseSensitive(cjson_root, "cmd");
    if (cjson_root == NULL)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            printf("[cjson] error: %s\n", error_ptr);
        }
        return;
    }
    cJSON *cjson_data = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cjson_root, "msg"), "data");
    cJSON *cjson_power = cJSON_GetObjectItemCaseSensitive(cjson_data, "1");
    cJSON *cjson_brightness = cJSON_GetObjectItemCaseSensitive(cjson_data, "4");
    cJSON *cjson_warmth = cJSON_GetObjectItemCaseSensitive(cjson_data, "3");

    /*
    if(cJSON_IsNumber(command) && command->valueint == 3)
    {
        //msg
    }
    else if(cJSON_IsNumber(command) && command->valueint == 10)
    {
        //feedback
    }
    */

    int power = -1;
    int brightness = -1;
    int warmth = -1;
    if (cJSON_IsNumber(cjson_power))
    {
        power = cjson_power->valueint;
    }

    if (cJSON_IsNumber(cjson_brightness))
    {
        brightness = cjson_brightness->valueint;
    }

    if (cJSON_IsNumber(cjson_warmth))
    {
        warmth = convert_to_kelvin(cjson_warmth->valueint);
    }
    cJSON_Delete(cjson_root);

    char ha_state[256];

    if (power >= 0 && brightness < 0 && warmth < 0)
    {
        snprintf(ha_state, sizeof(ha_state),
                 HA_PAYLOAD_POWER,
                 (power == 1 ? "ON" : "OFF"));
    }
    else if (brightness > 0 && warmth < 0)
    {
        snprintf(ha_state, sizeof(ha_state),
                 HA_PAYLOAD_BRIGHTNESS,
                 (power == 1 ? "ON" : "OFF"), brightness);
    }
    else if (brightness < 0 && warmth > 0)
    {
        snprintf(ha_state, sizeof(ha_state),
                 HA_PAYLOAD_WARMTH,
                 (power == 1 ? "ON" : "OFF"), warmth);
    }
    else
    {
        snprintf(ha_state, sizeof(ha_state),
                 HA_PAYLOAD,
                 (power == 1 ? "ON" : "OFF"), brightness, warmth);
    }

    printf("[BULB] translation: %s\n", ha_state);

    char state_topic[64];
    sprintf(state_topic, STATE_TOPIC, id);
    mosquitto_publish(mosq, NULL, state_topic, strlen(ha_state), ha_state, 0, false);
}

void send_bulb_packet(const int id, int power, int brightness, int warm)
{
    char buffer[512];

    long long sn = (long long)time(NULL) * 1000;
    int len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD,
                       power, warm, brightness, sn);

    if (send(device_sockets[id], buffer, len, 0) == SOCKET_ERROR)
    {
        fprintf(stderr, "Send failed: %d\n", WSAGetLastError());
    }
    else
    {
        printf("[HA] translation: %s\n", buffer);
    }
}
void send_bulb_packet_power(const int id, int power)
{
    char buffer[512];

    long long sn = (long long)time(NULL) * 1000;
    int len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD_POWER,
                       power, sn);

    if (send(device_sockets[id], buffer, len, 0) == SOCKET_ERROR)
    {
        fprintf(stderr, "Send failed: %d\n", WSAGetLastError());
    }
    else
    {
        printf("[HA] translation: %s\n", buffer);
    }
}
void send_bulb_packet_brightness(const int id, int brightness)
{
    char buffer[512];

    long long sn = (long long)time(NULL) * 1000;
    int len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD_BRIGHTNESS,
                       1, brightness, sn);

    if (send(device_sockets[id], buffer, len, 0) == SOCKET_ERROR)
    {
        fprintf(stderr, "Send failed: %d\n", WSAGetLastError());
    }
    else
    {
        printf("[HA] translation: %s\n", buffer);
    }
}
void send_bulb_packet_warmth(const int id, int warm)
{
    char buffer[512];

    long long sn = (long long)time(NULL) * 1000;
    int len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD_WARMTH,
                       1, warm, sn);

    if (send(device_sockets[id], buffer, len, 0) == SOCKET_ERROR)
    {
        fprintf(stderr, "Send failed: %d\n", WSAGetLastError());
    }
    else
    {
        printf("[HA] translation: %s\n", buffer);
    }
}

bool connected = 0;
void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    if (rc == 0)
    {
        for (int i = 0; i < device_count; i++)
        {
            // open_tcp_socket(devices_ip[i]); // socket should be open from scann method

            send_discovery_to_ha(mosq, i);
        }
        printf("Cozy lamp manager running.\n");
        connected = 1;
    }
    else
    {
        printf("Connect failed with code %d\n", rc);
    }
}

int main()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        printf("failed start up\n");
        Sleep(5000);
        return 1;
    }

    scan_for_bulbs();
    if (device_count == 0)
    {
        printf("failed fetch devices\n");
        Sleep(5000);
        return 1;
    }

    mosquitto_lib_init();
    mosq = mosquitto_new("cozy_lamp_manager", true, NULL);

    if (mosquitto_username_pw_set(mosq, "mqtt_usr", "safe_passage") != MOSQ_ERR_SUCCESS)
    {
        printf("Error: Could not set MQTT credentials\n");
        Sleep(5000);
        return 1;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, ha_state_message);

    if (mosquitto_connect(mosq, MQTT_BROKER_IP, 1883, 60) != MOSQ_ERR_SUCCESS)
    {
        printf("Error: Could not connect to MQTT Broker at %s\n", MQTT_BROKER_IP);
        Sleep(5000);
        return 1;
    }

    while (1)
    {
        mosquitto_loop(mosq, 1, 1);
        if (!connected)
        {
            Sleep(10);
            continue;
        }

        for (int i = 0; i < device_count; i++)
        {
            char buffer[2048];
            int bytes = recv(device_sockets[i], buffer, sizeof(buffer) - 1, 0);

            if (bytes > 0)
            {
                buffer[bytes] = '\0';
                bulb_command_message(mosq, i, buffer);
            }
            else if (bytes == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK)
                {
                    printf("Connection lost.\n");
                    // if need handle reconnection
                    return 1;
                }
            }
        }

        Sleep(10);
    }

    // Cleanup
    for (int i = 0; i < device_count; i++)
    {
        closesocket(device_sockets[i]);
    }

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    WSACleanup();
    return 0;
}