"""
    ESP NOW - Auto-forming mesh - ESP32 Nodes

    Date: 
        2023-10-15

    Author: 
        David Alexis <https://github.com/davealexis>

    Purpose: 
        Provides a base for creating projects with a self-organizing mesh of nodes using ESP Now
        using the ESP Now support added to MicroPython 1.21 (October 2023).

    TODO: The ESP Now send() method does not seem to be properly returning False if the intended target
    is not in the mesh. Add a mechanism to periodically send out a beacon to the mesh. All nodes
    will then track when the last beacon for each node was received. If a node has not reported in
    in a while, it will be removed from the mesh.
"""

import network
import aioespnow
import asyncio
import ubinascii
from time import sleep
import os
from datetime import datetime

BROADCAST_ADDR = b'\xff\xff\xff\xff\xff\xff'

# --------------------------------------------------------------------------------
class Mesh:

    is_in_mesh = False

    # ----------------------------------------------------------------------------
    def __init__(self):
        self.sta = network.WLAN(network.STA_IF)
        self.mac_address = ubinascii.hexlify(self.sta.config('mac'))
        self.is_in_mesh = False
        self.peers = {}
        self.mesh = aioespnow.AIOESPNow()

        print(f"My MAC address: {self.mac_address}")

    # ----------------------------------------------------------------------------
    async def start(self):
        self.mesh.active(True)
        self.mesh.add_peer(BROADCAST_ADDR)

        asyncio.create_task(self.beacon_task())
        asyncio.create_task(self.notify_task())
        await asyncio.create_task(self.receiver_task())

    # ----------------------------------------------------------------------------
    async def beacon_task(self):
        """
        Periodically send out a "hello" broadcast in order to auto-discover
        new nodes.
        """
        
        while not self.is_in_mesh:
            await self.broadcast("--hello--")
            await asyncio.sleep(1)

        print("Beacon task exiting")

    # ----------------------------------------------------------------------------
    async def receiver_task(self):
        async for sender_mac, message in self.mesh:
            print(message.decode())
            
            # Check whether the sender is already in our registered peer list.
            # If not, add it to the list.
            try:
                _ = self.mesh.get_peer(sender_mac)
            except:
                print(f"Adding peer {ubinascii.hexlify(sender_mac).decode()}")
                await self.add_peer(sender_mac)
                self.is_in_mesh = True

    # ----------------------------------------------------------------------------
    async def broadcast(self, data):
        """
        Send a broadcast to all nodes in the mesh.
        """

        await self.mesh.asend(BROADCAST_ADDR, data)

    # ----------------------------------------------------------------------------
    async def send_message(self, message, peer_address):
        """
        Send a message to a recipient node
        """

        while True:
            if await self.mesh.asend(peer_address, message):
                self.peers[peer_address].failure_count = 0
                break
            
            self.peers[peer_address].failure_count += 1
            print(f"Failed to send message to {peer_address} {self.peers[peer_address].failure_count} times")

            if self.peers[peer_address].failure_count >= 5:
                print(f"Removing peer {peer_address}")
                await self.remove_peer(peer_address)
                break

            await asyncio.sleep(1)
                
    # ----------------------------------------------------------------------------
    async def add_peer(self, peer_address):
        """
        Add a peer to the mesh.
        """

        try:
            _ = await self.mesh.get_peer(peer_address)
        except Exception as e:
            # Peer not found in the ESPNow peer list. Safe to add it.
            try:
                self.mesh.add_peer(peer_address)
                self.peers[peer_address] = Peer(peer_address)
            except Exception as e:
                print(f"Failed to add peer: {e}")

    # ----------------------------------------------------------------------------
    async def remove_peer(self, peer_address):
        """
        Remove a peer from the mesh.
        """
        try:
            await self.mesh.del_peer(peer_address)
            del self.peers[peer_address]
        except Exception as e:
            print(f"Failed to remove peer: {e}")
    
    # ----------------------------------------------------------------------------
    async def remove_all_peers(self):
        """
        Remove all peers from the mesh.
        """
        for peer in self.mesh.get_peers():
            await self.remove_peer(peer.mac)

    # ----------------------------------------------------------------------------
    async def notify_task(self):
        print("Peer notifier started")

        while True:
            peers = self.mesh.get_peers()

            for peer in peers:
                print(f"Notifying peer {ubinascii.hexlify(peer[0]).decode()}")
                await self.send_message("Hi",peer[0])
                
            await asyncio.sleep(5)

# --------------------------------------------------------------------------------
def wifi_reset():
    """
    Reset the WiFi radio to a known state. The WiFi can be in a weird state after
    a soft reset, so this will reset it to a known state.
    """

    sta = network.WLAN(network.STA_IF)
    sta.active(False)
    ap = network.WLAN(network.AP_IF)
    ap.active(False)

    sta.active(True)

    while not sta.active():
        sleep(0.1)

    # If running on an ESP8266, ensure that we're not connected to an access point.
    if os.uname().sysname == "esp8266":
        sta.disconnect()

        while sta.isconnected():
            sleep(0.2)

    # Disable power saving, which turns off the WiFi radio periodically. When this happens,
    # ESP Now can loose connections and be unreliable in receiving messages.
    sta.config(pm=sta.PM_NONE)



# --------------------------------------------------------------------------------
class Peer:
    def __init__(self, mac_address):
        """
        Initializes a new instance of the Peer class, which is used to track errors
        and connectivity status of a node.

        Parameters:
            mac_address (str): The MAC address of the node.

        Returns:
            None
        """

        self.mac_address = mac_address
        self.failure_count = 0
        self.last_seen = datetime.utcnow()