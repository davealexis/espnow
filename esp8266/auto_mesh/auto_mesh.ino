/** ---------------------------------------------------------------------------------------------------
    ESP NOW - Auto-forming mesh

    Date: 2021-09-21

    Author: David Alexis <https://github.com/davealexis>

    Purpose: Provides a base for creating projects with a self-organizing mesh of nodes using ESP Now.

    Description: This sketch provides the core functinality needed to build ESP8266-based nodes that
        auto-join a mesh of nearby nodes when powered up.
        Failed nodes will be removed from the mesh after a given number of failed communiation attempts.
        New nodes will automatically get re-joined to the active nodes when powered up.

        There were lots of examples showing how to communicate between nodes, but they all assume that
        the MAC addresses of every node are known up-front (so they can be added to the code). This didn't
        seem practical. It would be much better if the mesh is self-forming from auto-discovering peers.

        This sketch shows how to create a mesh of peers (no controller and worker nodes - all nodes are
        equal). But it would not be difficult to extend it to have a single controller node. Two possible
        auto-peering strategies in this architecture could be:
        1. New nodes still send a broadcast when they power up, but only the controller node would respond
           and add the node as a peer.
        2. The controller MAC address could be the only known address (can be a custom MAC address). New
           nodes would send a "peering request" message to the controller on startup, and the controller
           would add them as peers.
        In this one-to-many topology, only the controller knows about all the worker nodes, and would be
        the only one they can communicate to.

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

#include <ESP8266WiFi.h>
#include <espnow.h>

#define DEBUG

#define ESP_NOW_MAX_DATA_LEN 250
#define ESP_NOW_SEND_SUCCESS 0
#define MAX_PEERS 20

// Strings used in the code are defined up front with the PROGMEM macro. This causes
// the strings to be stored in flash instead of RAM.
const char HELLO_MSG_STR[] PROGMEM = "-- Hello? Anyone there? --";
const char ESP_NOW_INIT_FAILURE_MSG_STR[] PROGMEM = "ESP Now failed to initialize. Will restart in 3 seconds.";
const char INITIALIZED_MSG_STR[] PROGMEM = "ESP Now initialized";
const char PEER_ADDED_TEMPLATE_STR[] PROGMEM = "Added peer: %s. %d active peers.\n";
const char OUTGOING_MSG_TEMPLATE_STR[] PROGMEM = "%s Message # %d";

uint8_t broadcastAddress[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct peer_info
{
    char id[18];
    uint8_t address[6];
    int failures;
};

peer_info peers[MAX_PEERS] = {};
int peerCount = 0;

bool ledOn = false;
int messageCount = 0;
char myName[14];


/* ................................................................................................
 * onDataReceived() is the callback function that ESP Now calls whenever data is received from
 * another node.
 * The main thing we want to set up here is the auto-peering of incomming MAC addresses that we
 * have not seem before. We'll register the address as a peer with ESP Now, but also store it in
 * our own list of known peers. This enables sending messages to each individual peer using their
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
    if (!esp_now_is_peer_exist(senderMacAddress))
    {
        addPeer(senderMacAddress, macStr);
    }

    // Now that we have the incoming data parsed and we know who its coming from
    // we can route the message to a handler that can perform some sort of action.
    // TODO: Add call to handler to process received data.
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

    peers[peerIndex].failures++;

    #ifdef DEBUG
    Serial.printf("Message sent to: %s failed %d times\n", macStr, peers[peerIndex].failures);
    #endif

    if (peers[peerIndex].failures >= 4)
    {
        // Peer is dead. Remove.
        removePeer(peerIndex);
        peers[peerIndex].failures = 0;

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
    uint8_t result = esp_now_send(broadcastAddress,  (uint8_t *) message.c_str(), message.length());

    #ifdef DEBUG
    if (result != 0)
    {
        Serial.printf("Unknown error: %d\n", result);
    }
    #endif
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
    strcpy(peers[peerCount].id, id);

    for (int i = 0; i < 6; i++)
    {
        peers[peerCount].address[i] = macAddress[i];
    }

    esp_now_add_peer(macAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

    peerCount++;

    #ifdef DEBUG
    Serial.printf(PEER_ADDED_TEMPLATE_STR, id, peerCount);
    #endif
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
    esp_now_del_peer(peers[peerIndex].address);

    // Pack list to remove current item
    if (peerIndex < MAX_PEERS)
    {
        for (int p = peerIndex + 1; p < peerCount; p++)
        {
            strcpy(peers[peerIndex].id, peers[p].id);

            for (int a = 0; a < 6; a++)
            {
                peers[peerIndex].address[a] = peers[p].address[a];
            }
        }
    }

    // Wipe last entry
    memset(peers[peerCount - 1].id, 0, sizeof(peers[peerCount - 1].id));
    memset(peers[peerCount - 1].address, 0, sizeof(peers[peerCount - 1].address));

    peerCount--;

    #ifdef DEBUG
    Serial.printf("%d Peers\n", peerCount);
    #endif
}

/* ................................................................................................
 * findPeer() locates a peer, given it's MAC address, in our list of known peers. If it is not
 * found, -1 is returned.
 * ................................................................................................
 */
int findPeer(char *peerId)
{
    for (int i = 0; i < peerCount; i++)
    {
        if (strcmp(peerId, peers[i].id) == 0)
        {
            return i;
        }
    }

    return -1;
}

// ................................................................................................
void setup()
{
    #ifdef DEBUG
    Serial.begin(115200);
    delay(1000);
    #endif

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
    // If we have at least one peer, then let's send messages to them.
    if (peerCount != 0)
    {
        messageCount++;
        char msg[50];
        sprintf(msg, OUTGOING_MSG_TEMPLATE_STR, myName, messageCount);

        for (int i = 0; i < peerCount; i++)
        {
            sendMessage(msg, peers[i].address);
        }
    }
    else
    {
        #ifdef DEBUG
        Serial.println("Looking for peers");
        #endif

        broadcast(HELLO_MSG_STR);
    }

    delay(2000);
}
