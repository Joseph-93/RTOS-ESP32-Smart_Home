from django.urls import path
from . import views

urlpatterns = [
    path('', views.index, name='index'),
    path('add_device/', views.add_device, name='add_device'),
    path('scan_devices/', views.scan_devices, name='scan_devices'),
    path('delete_device/<str:device_name>/', views.delete_device, name='delete_device'),
    path('device/<str:device_name>/', views.device_view, name='device'),
    path('device/<str:device_name>/message-builder/', views.message_builder, name='message_builder'),
    path('device/<str:device_name>/<str:component_name>/', views.component_view, name='component'),
    path('api/<str:device_name>/info/', views.get_device_info, name='api_device_info'),
    
    # RGB Preset metadata API
    path('api/<str:device_name>/<str:component_name>/presets/', views.rgb_presets_list, name='rgb_presets_list'),
    path('api/<str:device_name>/<str:component_name>/presets/save/', views.rgb_preset_save, name='rgb_preset_save'),
    path('api/<str:device_name>/<str:component_name>/presets/<str:preset_name>/', views.rgb_preset_get, name='rgb_preset_get'),
    path('api/<str:device_name>/<str:component_name>/presets/<str:preset_name>/delete/', views.rgb_preset_delete, name='rgb_preset_delete'),
    path('api/<str:device_name>/<str:component_name>/presets/<str:preset_name>/rename/', views.rgb_preset_rename, name='rgb_preset_rename'),
]
