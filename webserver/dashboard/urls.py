from django.urls import path
from . import views

urlpatterns = [
    path('', views.index, name='index'),
    path('add_device/', views.add_device, name='add_device'),
    path('delete_device/<str:device_name>/', views.delete_device, name='delete_device'),
    path('device/<str:device_name>/', views.device_view, name='device'),
    path('device/<str:device_name>/<str:component_name>/', views.component_view, name='component'),
    path('api/<str:device_name>/info/', views.get_device_info, name='api_device_info'),
]
