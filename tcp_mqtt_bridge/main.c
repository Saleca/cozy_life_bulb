#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mosquitto.h>
#include "cjson/cJSON.h"

#define DEBUG_LOGS 0

#define TARGET_PORT 5555
#define SUBNET_PREFIX "192.168.1."
#define HA_IP SUBNET_PREFIX "120"
#define SLEEP_TIMEOUT_US 10000
#define SCAN_TIMEOUT_MS 250
#define RECONNECTION_TIMEOUT_S 60

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

void scan_network();

bool confirm_tcp_connection(int current_socket, int timeout_ms);
int start_tcp_connection(const char *ip);

bool reconnect_tcp_socket(const int id);
void close_tcp_socket(const int id);
void send_tcp_packet(const int id, int power, int brightness, int warm);
void check_tcp_packet(const int id);
void receive_tcp_packet(struct mosquitto *mqtt, const int id, const char *payload);

bool init_mqtt();
void mqtt_tick();
void on_mqtt_connect(struct mosquitto *mqtt, void *obj, int rc);
void send_mqtt_packet(const char *topic_base, const int id, const char *payload);
void receive_mqtt_packet(struct mosquitto *mqtt, void *obj, const struct mosquitto_message *msg);

void ha_discovery(struct mosquitto *mqtt, int id);

void get_device_name(char *dest, int id);
int get_device_id(const char *device_name);

uint16_t convert_to_kelvin(uint16_t input);
uint16_t convert_to_permille(uint16_t input);

struct mosquitto *mqtt = NULL;
int device_sockets[16];
char device_ips[16][16];
time_t device_reconnect_time[16];
int device_count = 0;
bool connected = 0;

void ha_discovery(struct mosquitto *mqtt, int id)
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
        mosquitto_subscribe(mqtt, NULL, set_topic, 0);
        send_mqtt_packet(AVAILABILITY_TOPIC, id, "online");
        free(config_payload);
    }
    cJSON_Delete(config_payload_cjson);
}

int main()
{
    scan_network();
    if (device_count == 0)
    {
        printf("failed to fetch devices\n");
        return 1;
    }

    if (!init_mqtt())
    {
        return 1;
    }

    while (!connected)
    {
        mqtt_tick();
    }

    while (1)
    {
        mqtt_tick();

        for (int i = 0; i < device_count; i++)
        {
            if (device_sockets[i] == -1)
            {
                if (!reconnect_tcp_socket(i))
                {
                    continue;
                }
            }

            check_tcp_packet(i);
        }
    }

    // Cleanup
    for (int i = 0; i < device_count; i++)
    {
        close(device_sockets[i]);
    }

    mosquitto_destroy(mqtt);
    mosquitto_lib_cleanup();

    return 0;
}

/* NETWORK */

int start_tcp_connection(const char *ip)
{
    int current_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (current_socket == -1)
    {
        return -1;
    }

    int flags = fcntl(current_socket, F_GETFL, 0);
    if (flags == -1)
    {
        close(current_socket);
        return -1;
    }
    fcntl(current_socket, F_SETFL, flags | O_NONBLOCK);

    int one = 1;
    setsockopt(current_socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TARGET_PORT);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(current_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            printf("errno: %s", strerror(errno));
            close(current_socket);
            return -1;
        }
    }
    return current_socket;
}

bool confirm_tcp_connection(int current_socket, int timeout_ms)
{
    if (current_socket < 0)
    {
        printf("[DEBUG] invalid socket %d\n", current_socket);
        return false;
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(current_socket, &wset);

    struct timeval tv = {.tv_sec = 0, .tv_usec = timeout_ms * 1000};

    int err = select(current_socket + 1, NULL, &wset, NULL, &tv);
    if (err <= 0)
    {
        if (err == 0)
        {
            printf("[DEBUG] Select timeout at %d\n", current_socket);
        }
        else
        {
            printf("[DEBUG] Select Error at %d: %s\n", current_socket, strerror(errno));
        }
        return false;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(current_socket, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0)
    {
        printf("[DEBUG] connection refused at %d\n", current_socket);
        return false;
    }

    int flags = fcntl(current_socket, F_GETFL, 0);
    fcntl(current_socket, F_SETFL, flags & ~O_NONBLOCK);
    return true;
}

void scan_network()
{
    int temp_sockets[256];
    for (int i = 0; i < 256; i++)
    {
        temp_sockets[i] = -1;
    }

    printf("Scanning network...\n");
    for (int i = 1; i < 255; i++)
    {
        char ip[16];
        snprintf(ip, sizeof(ip), "%s%d", SUBNET_PREFIX, i);

        temp_sockets[i] = start_tcp_connection(ip);
        usleep(500);
    }

    usleep(200 * 1000);

    for (int i = 1; i < 255; i++)
    {
        int current_socket = temp_sockets[i];

        if (current_socket < 0)
        {
            continue;
        }

        bool found = false;
        if (confirm_tcp_connection(current_socket, 1))
        {
            if (device_count < 16)
            {
                snprintf(device_ips[device_count], 16, "%s%d", SUBNET_PREFIX, i);
                device_sockets[device_count] = current_socket;
                device_count++;
                printf("Found light at: %s\n", device_ips[device_count - 1]);
                found = true;
            }
        }

        if (!found)
        {
            close(current_socket);
        }
    }
}

bool reconnect_tcp_socket(const int id)
{
    if (time(NULL) < device_reconnect_time[id])
    {
        return false;
    }

    int current_socket = start_tcp_connection(device_ips[id]);
    if (!confirm_tcp_connection(current_socket, SCAN_TIMEOUT_MS))
    {
        if (current_socket != -1)
        {
            close(current_socket);
        }
        device_reconnect_time[id] = time(NULL) + RECONNECTION_TIMEOUT_S;
        return false;
    }

    device_sockets[id] = current_socket;

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

    ssize_t sent_len = send(device_sockets[id], buffer, len, MSG_NOSIGNAL);
    if (sent_len < (ssize_t)len)
    {
        printf("Partial send: %zd of %d bytes\n", sent_len, len);
    }

    if (sent_len == -1)
    {
        if (errno == EPIPE)
        {
            printf("[TCP] Connection broken (EPIPE). Lamp disconnected.\n");
        }
        else
        {
            fprintf(stderr, "Send failed: %s\n", strerror(errno));
        }
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

    if (select(device_sockets[id] + 1, &read_set, NULL, NULL, &tv) <= 0)
    {
        close_tcp_socket(id);
        send_mqtt_packet(AVAILABILITY_TOPIC, id, "offline");
        return;
    }

#if DEBUG_LOGS
    printf("[HA] translation: %s\n", buffer);
#endif
}

void receive_tcp_packet(struct mosquitto *mqtt, const int id, const char *payload)
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

void check_tcp_packet(const int id)
{
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(device_sockets[id], &read_set);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000;

    if (select(device_sockets[id] + 1, &read_set, NULL, NULL, &timeout) <= 0)
    {
        return;
    }

    char buffer[2048];
    int bytes = recv(device_sockets[id], buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0)
    {
        buffer[bytes] = '\0';
        receive_tcp_packet(mqtt, id, buffer);
    }
    else if (bytes == 0 || (bytes == -1 && errno != EWOULDBLOCK && errno != EAGAIN))
    {
        close_tcp_socket(id);
        send_mqtt_packet(AVAILABILITY_TOPIC, id, "offline");
    }
}

void close_tcp_socket(const int id)
{
    close(device_sockets[id]);
    device_sockets[id] = -1;
}

/* MQTT */

bool init_mqtt()
{
    mosquitto_lib_init();
    mqtt = mosquitto_new("cozy_lamp_manager", true, NULL);

    if (mosquitto_username_pw_set(mqtt, "mqtt_usr", "safe_passage") != MOSQ_ERR_SUCCESS)
    {
        printf("Error: Could not set MQTT credentials\n");
        return false;
    }

    mosquitto_connect_callback_set(mqtt, on_mqtt_connect);
    mosquitto_message_callback_set(mqtt, receive_mqtt_packet);

    if (mosquitto_connect(mqtt, HA_IP, 1883, 60) != MOSQ_ERR_SUCCESS)
    {
        printf("Error: Could not connect to MQTT Broker at %s\n", HA_IP);
        return false;
    }
    return true;
}

void mqtt_tick()
{
    usleep(SLEEP_TIMEOUT_US);
    mosquitto_loop(mqtt, -1, 1);
}

void on_mqtt_connect(struct mosquitto *mqtt, void *obj, int rc)
{
    if (rc == 0)
    {
        for (int i = 0; i < device_count; i++)
        {
            ha_discovery(mqtt, i);
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
    mosquitto_publish(mqtt, NULL, topic, (int)strlen(payload), payload, 0, true);

#if DEBUG_LOGS
    printf("%s://%s.\n", device_ips[id], payload);
#endif
}

void receive_mqtt_packet(struct mosquitto *mqtt, void *obj, const struct mosquitto_message *msg)
{
    if (!msg->payload || msg->payloadlen == 0)
    {
        printf("no payload. (payload len:%d)\n", msg->payloadlen);
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

    char *payload = malloc(msg->payloadlen + 1);
    if (!payload)
    {
        return;
    }
    memcpy(payload, msg->payload, msg->payloadlen);
    payload[msg->payloadlen] = '\0';
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
    free(payload);

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