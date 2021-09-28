/** ---------------------------------------------------------------------------------------------------
    ESP NOW - Auto-forming mesh - ESP8266 Controller Node

    Date: 2021-09-26

    Author: David Alexis <https://github.com/davealexis>

    Purpose: Provides a base for creating projects with a self-organizing mesh of nodes using ESP Now.

    Description: This sketch provides the core functinality needed to build ESP8266-based nodes that
        auto-join a mesh of nearby nodes when powered up.
        Failed nodes will be removed from the mesh after a given number of failed communiation attempts.
        New nodes will automatically get re-joined to the active nodes when powered up.

        The mesh can contain both ESP8266 and ESP32 nodes.

        There were lots of examples showing how to communicate between nodes, but they all assume that
        the MAC addresses of every node are known up-front (so they can be added to the code). This didn't
        seem practical. It would be much better if the mesh is self-forming from auto-discovering peers.

        The architecture implemented here shows how to create a mesh of peers, with this sketch serving
        as the Controller. The auto-peering strategy is:
        
        Controller:
            The controller node listens for peering requests broadcast from new nodes. Other nodes may receive
            these broadcasts, but will ignore them. Only the controller responds.
            1. Adds the node's MAC address to its list of known nodes
            2. Send the node an Ack message to tell it that peering was done.

            Listens for event messages from nodes.
            1. Parses the event data
            2. Acts on the data - e.g. sends the data to gateway node (like a Raspberry Pi) for storage

            Sends command messages to nodes
            1. Go to sleep
            2. Reset
            3. ...

        ** Note ** The ESP Now library for the ESP8266 is different from the one for the ESP32, for some
        reason. This is why there are separate examples for each platform.

    Resources:
        Some resources that were instrumental in learning about ESP Now are:
        - https://espressif.com/sites/default/files/documentation/esp-now_user_guide_en.pdf
        - https://randomnerdtutorials.com/esp-now-esp32-arduino-ide
        - https://randomnerdtutorials.com/esp-now-esp8266-nodemcu-arduino-ide
        - https://github.com/atomic14/ESPNowSimpleSample
        - https://github.com/pcbreflux/espressif
        - https://www.instructables.com/How-to-Make-Multiple-ESP-Talk-Via-ESP-NOW-Using-ES

     ---------------------------------------------------------------------------------------------------
 */

#include <TaskScheduler.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

#define DEBUG

#define ESP_NOW_MAX_DATA_LEN 250
#define ESP_NOW_SEND_SUCCESS 0
#define MAX_NODES 20
#define PING_INTERVAL 5000

// Strings used in the code are defined up front with the PROGMEM macro. This causes
// the strings to be stored in flash instead of RAM.
const char ACK_MSG_STR[] PROGMEM = "ACK: Welcome!";
const char PING_MSG_STR[] PROGMEM = "PING";
const char ESP_NOW_INIT_FAILURE_MSG_STR[] PROGMEM = "ESP Now failed to initialize. Will restart in 3 seconds.";
const char INITIALIZED_MSG_STR[] PROGMEM = "ESP Now initialized";
const char PEER_ADDED_TEMPLATE_STR[] PROGMEM = "Added peer: %s. %d active peers.\n";
const char OUTGOING_MSG_TEMPLATE_STR[] PROGMEM = "%s Message # %d";

uint8_t broadcastAddress[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct node_info
{
    char id[18];
    uint8_t address[6];
    int failures;
};

node_info nodes[MAX_NODES] = {};
int nodeCount = 0;

const bool ledOn = false;
const bool ledOff = true;
const uint8_t ledPin = LED_BUILTIN;
int messageCount = 0;
char myName[14];

void pingHandler();

// Define tasks
Task pingTask(PING_INTERVAL, TASK_FOREVER, &pingHandler);
Scheduler taskRunner;

/* ................................................................................................
 *  pingHandler() is periodically called by the Task Scheduler in the background.
 *  It sends out pings to all known nodes. The onDataSent() handler will manage removing dead
 *  nodes when transmission attempts fail.
 * ................................................................................................
 */
void pingHandler()
{
    // Send ping to each known node
    if (nodeCount != 0)
    {
        blink(100);
        
        for (int i = 0; i < nodeCount; i++)
        {
            sendMessage(PING_MSG_STR, nodes[i].address);
        }

        blink(300);
    }
}

/* ................................................................................................
 * onDataReceived() is the callback function that ESP Now calls whenever data is received from
 * another node.
 * The main thing we want to set up here is the auto-peering of incoming MAC addresses that we
 * have not seem before. We'll register the address as a peer with ESP Now, but also store it in
 * our own list of known nodes. This enables sending messages to each individual node using their
 * recorded MAC address. No MAC addresses need to be known at compile time!
 * ................................................................................................
 */
void onDataReceived(uint8_t *senderMacAddress, uint8_t *data, uint8_t dataLen)
{
    // only allow a maximum of 250 characters in the message + a null terminating byte
    char buffer[ESP_NOW_MAX_DATA_LEN + 1];
    int msgLen = ESP_NOW_MAX_DATA_LEN < dataLen ? ESP_NOW_MAX_DATA_LEN : dataLen;
    strncpy(buffer, (const char *)data, msgLen);

    // make sure we are null terminated
    buffer[dataLen] = 0;

    // format the mac address
    char macStr[18];
    formatMacAddress(senderMacAddress, macStr, 18);

    #ifdef DEBUG
    Serial.printf("Received message from: %s - %s\n", macStr, buffer);
    #endif


    // If we don't know about the incoming mac address, let's peer up with it.
    if (nodeCount < MAX_NODES && !esp_now_is_peer_exist(senderMacAddress))
    {
        addPeer(senderMacAddress, macStr);
    }
    else
    {
        char *token = strtok(buffer, ":");

        if (strcmp(token, "PEER") == 0)
        {
            sendMessage(ACK_MSG_STR, senderMacAddress);
            
            #ifdef DEBUG
            Serial.printf("Sent Ack to known node %s\n", macStr);
            #endif
        }
    }

    // Now that we have the incoming data parsed and we know who its coming from
    // we can route the message to a handler that can perform some sort of action.
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

    int peerIndex = findPeer(macStr);

    if (peerIndex == -1)
    {
        return;
    }

    nodes[peerIndex].failures++;

    #ifdef DEBUG
    Serial.printf("Message sent to: %s failed %d times\n", macStr, nodes[peerIndex].failures);
    #endif

    if (nodes[peerIndex].failures >= 4)
    {
        // Peer is dead. Remove.
        removePeer(peerIndex);
        nodes[peerIndex].failures = 0;

        #ifdef DEBUG
        Serial.printf("Peer %s removed\n", macStr);
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

    int32_t result = esp_now_send(address,  (uint8_t *) message.c_str(), message.length());

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
        macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]
    );
}

/* ................................................................................................
 *  addPeer() registers a MAC address with ESP Now and adds its information to our list of known
 *  peers.
 *  With this, we can know at all times how many active peers are in our mesh.
 * ................................................................................................
 */
void addPeer(uint8_t *macAddress, char *id)
{
    strcpy(nodes[nodeCount].id, id);

    for (int i = 0; i < 6; i++)
    {
        nodes[nodeCount].address[i] = macAddress[i];
    }

    esp_now_add_peer(macAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

    nodeCount++;

    #ifdef DEBUG
    Serial.printf(PEER_ADDED_TEMPLATE_STR, id, nodeCount);
    #endif

    // Send ACK message to newly added node
    sendMessage(ACK_MSG_STR, macAddress);
}

/* ................................................................................................
 * removePeer() unregisters a MAC address from ESP Now and also removes the peer's info from our
 * list of known peers.
 * We need to pack the array of peers after removing one so that all the peer entries are contiguous
 * in the array.
 * ................................................................................................
 */
// Deletes a peer from the list of registered peers (the peers[] array).
void removePeer(int peerIndex)
{
    esp_now_del_peer(nodes[peerIndex].address);

    // Pack list to remove current item
    if (peerIndex < MAX_NODES)
    {
        for (int p = peerIndex + 1; p < nodeCount; p++)
        {
            strcpy(nodes[peerIndex].id, nodes[p].id);

            for (int a = 0; a < 6; a++)
            {
                nodes[peerIndex].address[a] = nodes[p].address[a];
            }
        }
    }

    // Wipe last entry
    memset(nodes[nodeCount - 1].id, 0, sizeof(nodes[nodeCount - 1].id));
    memset(nodes[nodeCount - 1].address, 0, sizeof(nodes[nodeCount - 1].address));

    nodeCount--;

    #ifdef DEBUG
    Serial.printf("%d Peers\n", nodeCount);
    #endif
}

/* ................................................................................................
 * findPeer() locates a peer, given it's MAC address, in our list of known peers. If it is not
 * found, -1 is returned.
 * ................................................................................................
 */
int findPeer(char *peerId)
{
    for (int i = 0; i < nodeCount; i++)
    {
        if (strcmp(peerId, nodes[i].id) == 0)
        {
            return i;
        }
    }

    return -1;
}

// ................................................................................................
void blink(int duration)
{
    digitalWrite(ledPin, ledOn);
    delay(duration);
    digitalWrite(ledPin, ledOff);
    delay(duration);
}

// ................................................................................................
void setup()
{
    #ifdef DEBUG
    Serial.begin(115200);
    delay(1000);
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
    taskRunner.addTask(pingTask);
    pingTask.enable();

    #ifdef DEBUG
    Serial.println("Controller starting up");
    #endif
}

/* ................................................................................................
 * Initially, we'll have no known peers. So we want to broadcast (advertise) ourselves to invite
 * other nodes in the mesh to peer with us. Once we have at least one peer, we don't need to
 * continue advertising. This is because even the nodes that missed our transmission will send
 * their own broadcast when they power up, and we'll receive that (and auto-peer with them).
 * ................................................................................................
 */
void loop()
{   
    // All that is needed here is a call to start up the task scheduler
    // engine, which takes care of continually running our main logic task
    // in the background.
    taskRunner.execute();
}
