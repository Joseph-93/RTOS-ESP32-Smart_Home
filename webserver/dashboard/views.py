from django.shortcuts import render, redirect
from django.http import JsonResponse
import json


# Simple in-memory device registry (just stores host/IP info)
devices = {}


def index(request):
    """Main dashboard view - just lists devices"""
    return render(request, 'dashboard/index.html', {'devices': devices})


def add_device(request):
    """Add a new ESP32 device (just stores the IP/host info)"""
    if request.method == 'POST':
        name = request.POST.get('name', '').strip()
        host = request.POST.get('host', '').strip()
        port = int(request.POST.get('port', 80))  # Default HTTP port
        
        if name and host:
            devices[name] = {
                'name': name,
                'host': host,
                'port': port
            }
        
        return redirect('index')
    
    return redirect('index')


def delete_device(request, device_name):
    """Delete an ESP32 device"""
    if request.method == 'POST':
        if device_name in devices:
            del devices[device_name]
    
    return redirect('index')


def get_device_info(request, device_name):
    """Get device connection info for client-side to use"""
    if device_name in devices:
        return JsonResponse(devices[device_name])
    return JsonResponse({'error': 'Device not found'}, status=404)


def device_view(request, device_name):
    """View for a specific ESP32 device - shows list of components"""
    if device_name not in devices:
        return JsonResponse({'error': 'Device not found'}, status=404)
    
    device_info = devices[device_name]
    
    # Browser will fetch components directly from ESP32
    return render(request, 'dashboard/device.html', {
        'device_name': device_name,
        'device': device_info,
        'components': []  # Placeholder - JavaScript will fetch from ESP32
    })


def component_view(request, device_name, component_name):
    """View for a specific component - shows actions and parameters"""
    if device_name not in devices:
        return JsonResponse({'error': 'Device not found'}, status=404)
    
    device_info = devices[device_name]
    
    return render(request, 'dashboard/component.html', {
        'device_name': device_name,
        'component_name': component_name,
        'device': device_info
    })
