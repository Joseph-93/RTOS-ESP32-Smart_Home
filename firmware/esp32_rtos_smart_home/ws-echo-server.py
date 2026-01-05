#!/usr/bin/env python3
import asyncio
import websockets

async def echo(websocket):
    print("Client connected")
    try:
        async for message in websocket:
            print(f"Received: {message}")
            await websocket.send(message)
            print("Echoed back")
    except websockets.exceptions.ConnectionClosed:
        print("Client disconnected")

async def main():
    print("WebSocket server listening on ws://10.0.0.189:8080")
    async with websockets.serve(echo, "10.0.0.189", 8080):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
