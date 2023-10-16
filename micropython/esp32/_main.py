import asyncio
import automesh

# --------------------------------------------------------------------------------
async def main():
    mesh = automesh.Mesh()
    await mesh.start()

# --------------------------------------------------------------------------------
automesh.wifi_reset()
asyncio.run(main())