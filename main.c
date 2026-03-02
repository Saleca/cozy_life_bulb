#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mosquitto.h>
#include "cjson/cJSON.h"

#define DEBUG_LOGS 0

#define TARGET_PORT 5555
#define SUBNET_PREFIX "192.168.1."
#define MQTT_BROKER_IP SUBNET_PREFIX "120"
#define SCAN_TIMEOUT_MS 500
#define RECONNECTION_TIMEOUT_S 10

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

bool init_network();
void scan_subnet();
void probe_ip(const char *ip);
SOCKET open_tcp_socket(const char *ip);
bool reconnect_tcp_socket(const int id);
void close_tcp_socket(const int id);
void send_tcp_packet(const int id, int power, int brightness, int warm);
void receive_tcp_packet(struct mosquitto *mosq, const int id, const char *payload);

bool init_mqtt();
void on_mqtt_connect(struct mosquitto *mosq, void *obj, int rc);
void send_mqtt_packet(const char *topic_base, const int id, const char *payload);
void receive_mqtt_packet(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);

void ha_discovery(struct mosquitto *mosq, int id);

void get_device_name(char *dest, int id);
int get_device_id(const char *device_name);

uint16_t convert_to_kelvin(uint16_t input);
uint16_t convert_to_permille(uint16_t input);

struct mosquitto *mosq = NULL;
SOCKET device_sockets[16];
char device_ips[16][16];
time_t device_reconnect_time[16];
int device_count = 0;
bool connected = 0;

void ha_discovery(struct mosquitto *mosq, int id)
{
    char device_id[32];
    get_device_name(device_id, id);

    char availability_topic[64];
    sprintf(availability_topic, AVAILABILITY_TOPIC, id);

    char set_topic[64];
    sprintf(set_topic, SET_TOPIC, id);

    char state_topic[64];
    sprintf(state_topic, STATE_TOPIC, id);

    cJSON *config_payload_cjson = cJSON_CreateObject();
    cJSON_AddStringToObject(config_payload_cjson, "schema", "json");
    cJSON_AddStringToObject(config_payload_cjson, "name", "Cozy white light");
    cJSON_AddStringToObject(config_payload_cjson, "unique_id", device_id);
    cJSON_AddStringToObject(config_payload_cjson, "command_topic", set_topic);
    cJSON_AddStringToObject(config_payload_cjson, "state_topic", state_topic);
    cJSON_AddBoolToObject(config_payload_cjson, "brightness", true);
    cJSON_AddNumberToObject(config_payload_cjson, "brightness_scale", 1000);
    const char *color_modes_array[] = {"color_temp"};
    cJSON_AddItemToObject(config_payload_cjson, "supported_color_modes", cJSON_CreateStringArray(color_modes_array, 1));
    cJSON_AddBoolToObject(config_payload_cjson, "color_temp_kelvin", true);
    cJSON_AddStringToObject(config_payload_cjson, "availability_topic", availability_topic);

    char *config_payload = cJSON_PrintUnformatted(config_payload_cjson);
    if (config_payload)
    {
        send_mqtt_packet(CONFIG_TOPIC, id, config_payload);
        mosquitto_subscribe(mosq, NULL, set_topic, 0);
        send_mqtt_packet(AVAILABILITY_TOPIC, id, "online");
        free(config_payload);
    }
    cJSON_Delete(config_payload_cjson);
}

int main()
{
    if (!init_network())
    {
        return 1;
    }

    scan_subnet();
    if (device_count == 0)
    {
        printf("failed fetch devices\n");
        return 1;
    }

    if (!init_mqtt())
    {
        return 1;
    }

    while (!connected)
    {
        Sleep(10);
        mosquitto_loop(mosq, 1, 1);
    }

    while (1)
    {
        Sleep(10);
        mosquitto_loop(mosq, 1, 1);

        for (int i = 0; i < device_count; i++)
        {
            if (device_sockets[i] == INVALID_SOCKET)
            {
                if (!reconnect_tcp_socket(i))
                {
                    continue;
                }
            }

            char buffer[2048];
            int bytes = recv(device_sockets[i], buffer, sizeof(buffer) - 1, 0);
            if (bytes > 0)
            {
                buffer[bytes] = '\0';
                receive_tcp_packet(mosq, i, buffer);
            }
            else if (bytes == 0 || (bytes == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
            {
                close_tcp_socket(i);
                send_mqtt_packet(AVAILABILITY_TOPIC, i, "offline");
            }
        }
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

/* NETWORK */
bool init_network()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        printf("Failed start up\n");
        return false;
    }
    return true;
}

void scan_subnet()
{
    printf("Scanning subnet %s0/255\n", SUBNET_PREFIX);
    for (int i = 0; i < 255; i++) // i=1
    {
        char current_test_ip[16];
        sprintf(current_test_ip, "%s%d", SUBNET_PREFIX, i);

#if DEBUG_LOGS
        printf("testing: %s\n", current_test_ip);
#endif
        probe_ip(current_test_ip);
    }
}

void probe_ip(const char *ip)
{
    SOCKET current_socket = open_tcp_socket(ip);
    if (current_socket == INVALID_SOCKET)
    {
        return;
    }

    strcpy(device_ips[device_count], ip);
    device_sockets[device_count] = current_socket;
    device_count++;
    printf("Found light at: %s\n", ip);
}

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

    connect(current_socket, (struct sockaddr *)&server, sizeof(server));

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(current_socket, &write_set);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = SCAN_TIMEOUT_MS * 1000;

    if (select(0, NULL, &write_set, NULL, &tv) <= 0)
    {
        closesocket(current_socket);
        return INVALID_SOCKET;
    }

    return current_socket;
}

bool reconnect_tcp_socket(const int id)
{
    if (time(NULL) < device_reconnect_time[id])
    {
        return false;
    }

    device_sockets[id] = open_tcp_socket(device_ips[id]);
    if (device_sockets[id] == INVALID_SOCKET)
    {
        device_reconnect_time[id] = time(NULL) + RECONNECTION_TIMEOUT_S;
        return false;
    }

    send_mqtt_packet(AVAILABILITY_TOPIC, id, "online");
    return true;
}

void send_tcp_packet(const int id, int power, int brightness, int warmth)
{
    char buffer[512];

    long long sn = (long long)time(NULL) * 1000;
    int len = 0;
    if (power >= 0 && brightness < 0 && warmth < 0)
    {
        len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD_POWER,
                       power, sn);
    }
    else if (brightness > 0 && warmth < 0)
    {
        len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD_BRIGHTNESS,
                       1, brightness, sn);
    }
    else if (brightness < 0 && warmth > 0)
    {
        len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD_WARMTH,
                       1, warmth, sn);
    }
    else
    {
        len = snprintf(buffer, sizeof(buffer),
                       BULB_PAYLOAD,
                       power, warmth, brightness, sn);
    }

    if (send(device_sockets[id], buffer, len, 0) == SOCKET_ERROR)
    {
        fprintf(stderr, "Send failed: %d\n", WSAGetLastError());
        close_tcp_socket(id);
        send_mqtt_packet(AVAILABILITY_TOPIC, id, "offline");
        return;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(device_sockets[id], &read_set);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = SCAN_TIMEOUT_MS * 1000;

    if (select(0, &read_set, NULL, NULL, &tv) <= 0)
    {
        close_tcp_socket(id);
        send_mqtt_packet(AVAILABILITY_TOPIC, id, "offline");
        return;
    }

#if DEBUG_LOGS
    printf("[HA] translation: %s\n", buffer);
#endif
}

void receive_tcp_packet(struct mosquitto *mosq, const int id, const char *payload)
{
#if DEBUG_LOGS
    printf("[BULB] message: %s", payload);
#endif

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
    if (cJSON_IsNumber(cjson_command) && cjson_command->valueint == 3)
    {
        // msg
    }
    else
    if(cJSON_IsNumber(cjson_command) && cjson_command->valueint == 10)
    {
        return;
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

    send_mqtt_packet(STATE_TOPIC, id, ha_state);
}

void close_tcp_socket(const int id)
{
    closesocket(device_sockets[id]);
    device_sockets[id] = INVALID_SOCKET;
}

/* MQTT */

bool init_mqtt()
{
    mosquitto_lib_init();
    mosq = mosquitto_new("cozy_lamp_manager", true, NULL);

    if (mosquitto_username_pw_set(mosq, "mqtt_usr", "safe_passage") != MOSQ_ERR_SUCCESS)
    {
        printf("Error: Could not set MQTT credentials\n");
        return false;
    }

    mosquitto_connect_callback_set(mosq, on_mqtt_connect);
    mosquitto_message_callback_set(mosq, receive_mqtt_packet);

    if (mosquitto_connect(mosq, MQTT_BROKER_IP, 1883, 60) != MOSQ_ERR_SUCCESS)
    {
        printf("Error: Could not connect to MQTT Broker at %s\n", MQTT_BROKER_IP);
        return false;
    }
    return true;
}

void on_mqtt_connect(struct mosquitto *mosq, void *obj, int rc)
{
    if (rc == 0)
    {
        for (int i = 0; i < device_count; i++)
        {
            ha_discovery(mosq, i);
        }
        connected = 1;
        printf("Cozy lamp manager running.\n");
    }
    else
    {
        printf("Connect failed with code %d\n", rc);
    }
}

void send_mqtt_packet(const char *topic_base, const int id, const char *payload)
{
    char topic[64];
    sprintf(topic, topic_base, id);
    mosquitto_publish(mosq, NULL, topic, (int)strlen(payload), payload, 0, true);

#if DEBUG_LOGS
    printf("%s://%s.\n", device_ips[id], payload);
#endif
}

void receive_mqtt_packet(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    if (!msg->payload)
    {
        return;
    }

    int id = get_device_id(msg->topic);

    if (id < 0)
    {
        printf("failled to get device id: %s\n", msg->topic);
        return;
    }
    else if (id >= device_count)
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

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state) && (state->valuestring != NULL))
    {
        if (strcmp(state->valuestring, "OFF") == 0)
        {
            power = 0;
        }
        else
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
    }

    cJSON *warm = cJSON_GetObjectItemCaseSensitive(root, "color_temp");
    if (cJSON_IsNumber(warm))
    {
        warmth = convert_to_permille(warm->valueint);
    }

    cJSON_Delete(root);

    send_tcp_packet(id, power, brightness, warmth);
}

/* HELPERS */

void get_device_name(char *dest, int id)
{
    sprintf(dest, DEVICE_NAME, id);
}

int get_device_id(const char *device_name)
{
    const char *ptr = strstr(device_name, DEVICE_NAME_PREFIX);

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