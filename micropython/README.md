# ESP Now Auto-Forming Mesh with MicroPython

**Date:** 2023-10-15

**Author:**  David Alexis <https://github.com/davealexis>



## Purpose

Provides a base for creating projects with a self-organizing mesh of nodes using ESP Now using the ESP Now support added to MicroPython 1.21.



## Description

The 1.21 release of MicroPython in October 2023 included the exciting addition of ESP Now support!

The code in this project is a translation of the older Arduino C/C++ version of my ESP Now sample sketches.

It provides the AutoMesh class that encaptulates the capability for nodes to automatically discover peers and join a mesh when powered up. Failed nodes will be removed from the mesh after a given number of failed communication attempts. New nodes will automatically get re-joined to the mesh when powered up.

An ESP Now mesh can contain both ESP8266 and ESP32 nodes. However, the topology and architecture of a mesh containing ESP8266 nodes will need to be limited to a Controller/Worker topology since the ESP Now capabilities are limited on the ESP8266 and the capabilities delivered in the 1.21 release of MicroPython on the ESP8266 do not support broadcasting.

### Why Create This Sample

There are lots of ESP Now examples showing how to communicate between nodes, but they all assume that
the MAC addresses of every node are known up-front (so they can be added to the code).  Other samples show indicate that the only way for nodes to discover each other is for each node set itself up as a WiFi access point. Other nodes would then need to scan for access points matching a given naming pattern and add them to the mesh. 

This didn't seem practical. It would be much better if the mesh could self-form from auto-discovering peers.

This code shows how to create a mesh of peers, rather than a hub/spoke topology with a dedicated controller node. But it would not be difficult to extend it to have a single controller node. Two possible auto-peering strategies in the hub/spoke topology could be:
1. New nodes send a broadcast when they power up, and only the controller node would respond
    and add the node as a peer. The controller knows about all the nodes, but the nodes only know about the controller. Alternatively, the controller can broadcast the list of known nodes out to all nodes, allowing them to subsequently perform peer-to-peer communication (e.g. depending on a service/capability advertised by a node).
2. The controller MAC address could be the only known address (can be a custom MAC address). New
    nodes would send a "peering request" message to the controller on startup, and the controller
    would add them as peers.
    In this one-to-many topology, only the controller knows about all the worker nodes, and would be
    the only one they can communicate to.

## Project Structure

- **main.py:**  This is the entry point of the code. It imports the automesh.py code an starts up the mesh using the Mesh class.
- **automesh.py:** This file contains the actual mesh functionality encapsulated in the Mesh class. All that is required is the create an instance of Mesh, then call the `start()` method. Replace the contents of the `Mesh.notify_task()` method with code that implements your desired node functionality.



----



## TODO

- ESP32 mesh controller with ESP8266 nodes
- Task to automatically remove peers that have not been seen in a while
    - Each node will periodically broadcast an "I'm alive" message
    - On receipt of this message, each node will refresh the "last seen" time for the node in it's own registration list
    - The peer management Task will remove any nodes that have not been seen in the last N ping period