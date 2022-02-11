/** ---------------------------------------------------------------------------------------------------
    ESP NOW - Controller/Worker Topology - ESP8266 Worker Node

    Date: 2021-09-27

    Author: David Alexis <https://github.com/davealexis>

    Purpose: Provides a base for creating IoT projects based on the controller/worker topology. This
        sketch provides the core Worker node functionality, and can be expanded or modified to suit.

    Description: This sketch provides the core functinality needed to build ESP8266-based nodes that
        auto-join a network with a controller node when powered up.
        Failed nodes will be removed from the network after a given number of failed communiation attempts.
        New nodes will automatically get re-joined to the controller when powered back up.

        The network can contain both ESP8266 and ESP32 nodes.

        There were lots of examples showing how to communicate between nodes, but they all assume that
        the MAC addresses of every node are known up-front (so they can be added to the code). This didn't
        seem practical. It would be much better if the mesh is self-forming from auto-discovering peers.

        The architecture implemented here shows how to create a topology with a controller and many
        worker nodes, with this sketch serving as the worker node. The auto-peering strategy is:

        The worker node needs to perform two main actions:
        1. Send a peering request to the Controller node on startup. It keeps sending the request
            broadcast until the Controller responds with an ACK message, indicating that peering
            was successful.
        2. Perform its main duty of periodically sending data to the Controller. This could be data
            read from sensors, for example.
        3. Listen for incoming notifications and commands from the Controller. These are mostly
            Ping requests that can be ignored, since ESP Now takes care of letting the controller
            know if the Node received the transmission. The controller can potentially send commands
            to tell the Node to do things like reset or do an immediate sensor reading.

        ** Note ** The ESP Now library for the ESP8266 is different from the one for the ESP32, for some
        reason. This is why there are separate examples for each platform.

     ---------------------------------------------------------------------------------------------------
 */

#include <TaskScheduler.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

#define DEBUG

#define ESP_NOW_MAX_DATA_LEN 250
#define ESP_NOW_SEND_SUCCESS 0
#define MAX_NODES 20
#define PROCESSING_INTERVAL 100
#define SENDING_INTERVAL 2000

// Strings used in the code are defined up front with the PROGMEM macro. This causes
// the strings to be stored in flash instead of RAM.
const char ACK_CMD_STR[] PROGMEM = "ACK";
const char PING_CMD_STR[] PROGMEM = "PING";
const char HELLO_MSG_STR[] PROGMEM = "PEER: Anyone there?";
const char ESP_NOW_INIT_FAILURE_MSG_STR[] PROGMEM = "ESP Now failed to initialize. Will restart in 3 seconds.";
const char INITIALIZED_MSG_STR[] PROGMEM = "ESP Now initialized";
const char PEER_ADDED_TEMPLATE_STR[] PROGMEM = "Added peer: %s. %d active peers.\n";
const char OUTGOING_MSG_TEMPLATE_STR[] PROGMEM = "%s Message # %d";

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
char buffer[ESP_NOW_MAX_DATA_LEN + 1];
uint8_t controllerAddress[6];
uint8_t incomingMacAddress[6];
int failures;
bool isConnected = false;
bool messageReceived = false;
int msgLen;

const bool ledOn = false;
const bool ledOff = true;
const uint8_t ledPin = LED_BUILTIN;
int messageCount = 0;
char myName[14];

void incomingMessageHandler();
void dataSenderHandler();

// Define tasks
Task receiveTask(PROCESSING_INTERVAL, TASK_FOREVER, &incomingMessageHandler);
Task sendTask(SENDING_INTERVAL, TASK_FOREVER, &dataSenderHandler);
Scheduler taskRunner;


// ................................................................................................
void blink(int duration)
{
    digitalWrite(ledPin, ledOn);
    delay(duration);
    digitalWrite(ledPin, ledOff);
    delay(duration);
}

/* ................................................................................................
 * onDataReceived() is the callback function that ESP Now calls whenever data is received from
 * another node.
 * Things we want to do when we receive data:
 * 1. If we received an ACK message, take the incoming MAC address as the Controller's address. 
 *    Also mark ourselves as connected. The MAC address of the Controller does not need to be 
 *    known at compile time!
 * 2. If we reveived a command or notification, act on it.
 * ................................................................................................
 */
void onDataReceived(uint8_t *senderMacAddress, uint8_t *data, uint8_t dataLen)
{
    // Only allow a maximum of 250 characters in the message + a null terminating byte
    
    msgLen = ESP_NOW_MAX_DATA_LEN < dataLen ? ESP_NOW_MAX_DATA_LEN : dataLen;
    strncpy(buffer, (const char *)data, msgLen);

    // Make sure the data is null terminated, otherwise we'll have big trouble.
    buffer[msgLen] = 0;

    for (int i = 0; i < 6; i++)
    {
        incomingMacAddress[i] = senderMacAddress[i];
    }

    // Now that we have the incoming data parsed and we know who its coming from
    // we can route the message to a handler that can perform some sort of action.
    // The background Task is triggered by setting messageReceived to true.
    messageReceived = true;
}

/* ................................................................................................
 *  onDataSent() is the callback function called by ESP Now after a transmission is sent.
 *  It notifies us whether the transmission succeeded or failed.
 *  The main thing we want to set up here is auto-removing a peer after N failed transmission
 *  attempts. The peer will auto-peer if it comes back online anyway. But removing failed peers
 *  lets us track the number of active peers in the mesh.
 * ................................................................................................
 */
void onDataSent(uint8_t *macAddr, uint8_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        // We can handle success here. But in this case, we're just going to return and do nothing.
        return;
    }

    // Transmission was not successful.
    // This code demonstrates how to handle a given number of failures to any given node,
    // after which we'll assume the node is dead and remove it from our peer list.
    // If it comes back online, it will automatically get re-peered with all active nodes.
    char macStr[18];
    formatMacAddress(macAddr, macStr, 18);
    failures++;

    #ifdef DEBUG
    Serial.printf("Message sent to controller at %s failed %d times\n", macStr, failures);
    #endif

    if (failures >= 4)
    {
        // Controller is dead. Remove.
        failures = 0;
        isConnected = false;

        #ifdef DEBUG
        Serial.println("Unpeered from controller");
        #endif
    }
}

/* ................................................................................................
 *  broadcast() ise used to blast a message out to all nodes within range, regardless of whether
 *  they're already peered.
 *  We will use this for new nodes to "advertise" themselves on startup. Any nodes receiving the
 *  broadcast will peer up with the new node.
 *  ...............................................................................................
 */
void broadcast(const String &message)
{
    // This will broadcast a message to everyone in range
    sendMessage(message, broadcastAddress);
}

/* ................................................................................................
 *  sendMessage() is used to transmit data to a specific node using its MAC address.
 * ................................................................................................
 */
void sendMessage(const String &message, uint8_t *address)
{
    // We need to ensure that the address we're sending to is registered with ESP Now as a peer.
    // This is required for ESP Now to be able to communicate to the address.
    if (!esp_now_is_peer_exist(address))
    {
        esp_now_add_peer(address, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
        char macStr[18];
        formatMacAddress(address, macStr, 18);

        #ifdef DEBUG
        Serial.printf("Added peer: %s\n", macStr);
        #endif
    }

    int32_t result = esp_now_send(address, (uint8_t *)message.c_str(), message.length());

    #ifdef DEBUG
    if (result != 0)
    {
        Serial.printf("Unknown error: %d\n", result);
    }
    #endif
}

// ................................................................................................
void formatMacAddress(const uint8_t *macAddress, char *buffer, int maxLength)
{
    snprintf(
        buffer,
        maxLength,
        "%02x:%02x:%02x:%02x:%02x:%02x",
        macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
}

/* ................................................................................................
 * incomingMessageHandler() is the background Task (managed by the TaskScheduler library) that
 * waits for the `messageReceived` flag to get set. It then processes the data in the `buffer`
 * variable and resets the `messageReceived` flag to false.
 * The processing of incoming data is done this way because the code crashes if certain things
 * are done in the onDataReceived() handler called by ESP Now. Not sure why this is. Blinking an
 * LED is an example of an action that would crash if done in onDataReceived().
 * ................................................................................................
 */
void incomingMessageHandler()
{
    if (messageReceived)
    {
        // Format the mac address as a string of hex digits for easier reading by humans.
        char macStr[18];
        formatMacAddress(incomingMacAddress, macStr, 18);
        messageReceived = false;

        // The protocol implemented here is to have the first few characters on the incoming
        // string data - up until the ":" character is encountered - is interpreted as the
        // message type. We understand the "ACK" and "PING" message types. Anything else
        // gets treated as plain message.
        char *token = strtok(buffer, ":");

        if (strcmp(token, "ACK") == 0)
        {
            #ifdef DEBUG
            Serial.printf("Yay! Controller %s peered up\n", macStr);
            #endif
    
            for (int i = 0; i < 6; i++)
            {
                controllerAddress[i] = incomingMacAddress[i];
            }
    
            isConnected = true;

            blink(100);
            blink(100);
            blink(100);
        }
        else if (strcmp(token, "PING") == 0)
        {
            #ifdef DEBUG
            Serial.println("Controller sent a ping");
            #endif

            blink(50);
            blink(50);
            blink(50);
            blink(50);
        }
        else
        {
            #ifdef DEBUG
            Serial.printf("Message: %s\n", token);
            #endif
            
            blink(250);
            blink(100);
            blink(100);
        }
    }
}

/* ................................................................................................
 * dataSenderHandler() is the background Task handler that will periodically send data
 * (e.g. sensor data) back to the Controller. It is also responsible for initially sending a
 * peering request to the Controller on startup.
 * ................................................................................................
 */
void dataSenderHandler()
{
    // If we're connected to the Controller, send messages to it.
    if (isConnected == true)
    {
        digitalWrite(ledPin, ledOn);
        
        messageCount++;
        char msg[50];
        sprintf(msg, OUTGOING_MSG_TEMPLATE_STR, myName, messageCount);

        sendMessage(msg, controllerAddress);

        delay(500);
        digitalWrite(ledPin, ledOff);
    }
    else
    {
        #ifdef DEBUG
        Serial.println("Looking for Controller...");
        #endif

        blink(200);
        blink(200);
        broadcast(HELLO_MSG_STR);
        blink(200);
        blink(200);
    }
}

// ................................................................................................
void setup()
{
    #ifdef DEBUG
    Serial.begin(115200);
    while (!Serial) ;
    #endif

    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, ledOff);

    // Set WiFi in station mode but disconnected.
    // WiFi must be disconnected in order for ESP Now to be used.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Generate a node name using the unique chip ID of the ESP board.
    snprintf(myName, 13, "Node-%08X", ESP.getChipId());

    #ifdef DEBUG
    Serial.printf("\nName: %s\nMAC Address: ", myName);
    Serial.println(WiFi.macAddress());
    #endif

    // Startup ESP Now
    if (esp_now_init() == 0)
    {
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
        esp_now_register_recv_cb(onDataReceived);
        esp_now_register_send_cb(onDataSent);

        #ifdef DEBUG
        Serial.println(INITIALIZED_MSG_STR);
        #endif
    }
    else
    {
        #ifdef DEBUG
        Serial.println(ESP_NOW_INIT_FAILURE_MSG_STR);
        #endif

        delay(3000);
        ESP.restart();
    }

    // The broadcast address is pre-registered as a peer with ESP Now so that we don't
    // have to perform a check (and possibly add) every time we call the broadcast() function.
    // We'll use the Combo role so that every node can both send and receive data.
    esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, 0, NULL, 0);

    // Initialize and start our background Task.
    taskRunner.init();
    taskRunner.addTask(receiveTask);
    taskRunner.addTask(sendTask);
    receiveTask.enable();
    sendTask.enable();

    #ifdef DEBUG
    Serial.println("Node starting up");
    #endif
}

// ................................................................................................
void loop()
{
    // All that is needed here is a call to start up the task scheduler
    // engine, which takes care of continually running our main logic task
    // in the background.
    taskRunner.execute();
}
