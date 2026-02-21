from django.shortcuts import render, redirect
from django.http import JsonResponse
import json
import socket
import logging

logger = logging.getLogger(__name__)

# Simple in-memory device registry (just stores host/IP info)
devices = {}


def scan_devices(request):
    """Scan the local network for ESP32 devices via mDNS and auto-add them."""
    log = []  # Collect log lines to return to the browser

    def dbg(msg):
        logger.info(msg)
        log.append(msg)

    try:
        from zeroconf import Zeroconf, ServiceBrowser, ServiceListener
        import zeroconf as zc_module
        dbg(f"zeroconf version: {zc_module.__version__}")
    except ImportError as e:
        return JsonResponse({'error': f'zeroconf not installed: {e}'}, status=500)

    import time

    # Log which local interfaces zeroconf will use
    try:
        hostname = socket.gethostname()
        local_ips = socket.getaddrinfo(hostname, None)
        local_ip_strs = list({r[4][0] for r in local_ips if ':' not in r[4][0]})
        dbg(f"Local host: {hostname}, IPs: {local_ip_strs}")
    except Exception as e:
        dbg(f"Could not resolve local IPs: {e}")

    found = {}
    raw_names = []

    class Listener(ServiceListener):
        def add_service(self, zc, type_, name):
            dbg(f"[mDNS] add_service called: name={name!r} type={type_!r}")
            raw_names.append(name)
            info = zc.get_service_info(type_, name)
            if info is None:
                dbg(f"  -> get_service_info returned None for {name!r}")
                return
            dbg(f"  -> addresses={info.addresses!r} port={info.port} server={info.server!r}")
            if info.addresses:
                ip = socket.inet_ntoa(info.addresses[0])
                port = info.port
                device_name = name.replace(f'.{type_}', '').replace('._ws._tcp', '')
                dbg(f"  -> resolved as device_name={device_name!r} ip={ip} port={port}")
                found[device_name] = {'ip': ip, 'port': port}
            else:
                dbg(f"  -> no addresses in service info, skipping")

        def remove_service(self, zc, type_, name):
            dbg(f"[mDNS] remove_service: {name!r}")

        def update_service(self, zc, type_, name):
            dbg(f"[mDNS] update_service: {name!r}")

    dbg("Starting Zeroconf and ServiceBrowser for _ws._tcp.local. ...")
    # Wi-Fi interface is 10.0.0.x - bind explicitly so multicast goes out the right NIC
    lan_ip = next((ip for ip in local_ip_strs if ip.startswith('10.')), None)
    dbg(f"Binding zeroconf to interface: {lan_ip or 'default (all interfaces)'}")

    from zeroconf import Zeroconf, ServiceBrowser, ServiceListener
    zc_kwargs = {'interfaces': [lan_ip]} if lan_ip else {}
    zc = Zeroconf(**zc_kwargs)
    browser = ServiceBrowser(zc, '_ws._tcp.local.', Listener())
    dbg("Waiting 5 seconds for mDNS responses...")
    time.sleep(5)
    zc.close()
    dbg(f"Scan complete. raw callbacks: {len(raw_names)}, resolved: {list(found.keys())}")

    # Fallback: if service browser found nothing, try resolving known hostnames directly
    if not found:
        dbg("Service browser found nothing - trying direct hostname fallback...")
        for hostname_candidate in ['esp32.local']:
            try:
                ip = socket.gethostbyname(hostname_candidate)
                dbg(f"  -> {hostname_candidate} resolved to {ip} via OS DNS/mDNS")
                device_name = hostname_candidate.replace('.local', '')
                found[device_name] = {'ip': ip, 'port': 80}
            except socket.gaierror as e:
                dbg(f"  -> {hostname_candidate} did not resolve: {e}")

    added = []
    for name, info in found.items():
        if name not in devices:
            devices[name] = {'name': name, 'host': info['ip'], 'port': info['port']}
            added.append(name)
            dbg(f"Added new device: {name} -> {info['ip']}:{info['port']}")
        else:
            dbg(f"Device already registered: {name}")

    return JsonResponse({'found': list(found.keys()), 'added': added, 'log': log})


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


def message_builder(request, device_name):
    """Message builder tool for creating WebSocket/executeMessage JSON"""
    if device_name not in devices:
        return JsonResponse({'error': 'Device not found'}, status=404)
    
    device_info = devices[device_name]
    return render(request, 'dashboard/message_builder.html', {
        'device_name': device_name,
        'device': device_info
    })

