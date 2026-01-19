from channels.routing import URLRouter
from django.urls import path
from . import consumers

websocket_urlpatterns = [
    path('ws/param_updates/', consumers.ParameterUpdateConsumer.as_asgi()),
]
