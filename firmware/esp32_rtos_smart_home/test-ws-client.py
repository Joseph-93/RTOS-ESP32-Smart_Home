#!/usr/bin/env python3
import asyncio
import websockets

async def test_websocket():
    url = "ws://10.0.0.156:1235"
    # Query channels 19-99 to get remaining saved messages
    channels = ','.join([f'{{"obj":"Third Party Transmitter","param":"Saved Messages","sub":0,"channel":{i}}}' for i in range(19, 100)])
    message = f'{{"jsonrpc":"2.0","method":"get","params":[{channels}],"id":3679}}'
    
    print(f"Connecting to {url}")
    try:
        async with websockets.connect(url) as websocket:
            print("Connected!")
            print(f"Sending: {repr(message)}")
            await websocket.send(message)
            print("Message sent")
            
            # Wait for response (10 seconds timeout)
            try:
                response = await asyncio.wait_for(websocket.recv(), timeout=10.0)
                print(f"Received: {response}")
            except asyncio.TimeoutError:
                print("No response received within 10 seconds")
                
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    asyncio.run(test_websocket())
