"""
WebSocket consumer for real-time parameter updates
"""
import json
from channels.generic.websocket import AsyncWebsocketConsumer


class ParameterUpdateConsumer(AsyncWebsocketConsumer):
    async def connect(self):
        """Handle WebSocket connection"""
        await self.accept()
        self.subscriptions = set()  # Track which device/component pairs we're subscribed to
        print(f"[WS] Client connected: {self.channel_name}")

    async def disconnect(self, close_code):
        """Handle WebSocket disconnection"""
        # Leave all subscription groups
        for group_name in self.subscriptions:
            await self.channel_layer.group_discard(group_name, self.channel_name)
        print(f"[WS] Client disconnected: {self.channel_name}")

    async def receive(self, text_data):
        """Handle incoming WebSocket messages from browser"""
        try:
            data = json.loads(text_data)
            action = data.get('action')
            
            if action == 'subscribe':
                device = data.get('device')
                component = data.get('component')
                
                if device and component:
                    group_name = f"param_updates_{device}_{component}"
                    await self.channel_layer.group_add(group_name, self.channel_name)
                    self.subscriptions.add(group_name)
                    print(f"[WS] Subscribed to {group_name}")
                    
                    await self.send(text_data=json.dumps({
                        'type': 'subscription_confirmed',
                        'device': device,
                        'component': component
                    }))
                    
            elif action == 'unsubscribe':
                device = data.get('device')
                component = data.get('component')
                
                if device and component:
                    group_name = f"param_updates_{device}_{component}"
                    await self.channel_layer.group_discard(group_name, self.channel_name)
                    self.subscriptions.discard(group_name)
                    print(f"[WS] Unsubscribed from {group_name}")
                    
                    await self.send(text_data=json.dumps({
                        'type': 'unsubscription_confirmed',
                        'device': device,
                        'component': component
                    }))
                    
        except json.JSONDecodeError:
            print(f"[WS] Invalid JSON received: {text_data}")
        except Exception as e:
            print(f"[WS] Error handling message: {e}")

    async def param_update(self, event):
        """Handle parameter update broadcast from channel layer"""
        # Forward the update to the WebSocket client
        await self.send(text_data=json.dumps({
            'type': 'param_update',
            'component': event['component'],
            'param_type': event['param_type'],
            'idx': event['idx'],
            'row': event['row'],
            'col': event['col'],
            'value': event['value']
        }))
