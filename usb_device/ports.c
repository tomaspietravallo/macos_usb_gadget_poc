#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "ports.h"

int valid_usb_port(const char *port)
{
    if (strncmp(port, "usb-drd", 7) != 0 || !isdigit((unsigned char)port[7])) return 0;
    for (const char *p = port + 8; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

static int gadget_present(const char *port)
{
    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("gay_bowser_usbgadget"), &iter) != KERN_SUCCESS) return 0;
    int found = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        io_string_t path;
        if (IORegistryEntryGetPath(service, kIOServicePlane, path) == KERN_SUCCESS &&
                strstr(path, port)) found = 1;
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return found;
}

static const char *current_function(const char *port)
{
    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOUSBDeviceInterface"), &iter) != KERN_SUCCESS) return "unknown";
    const char *function = "unknown";
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        io_string_t path;
        io_name_t name;
        if (IORegistryEntryGetPath(service, kIOServicePlane, path) == KERN_SUCCESS &&
                strstr(path, port) && IORegistryEntryGetName(service, name) == KERN_SUCCESS) {
            if (strcmp(name, "MyUSBData") == 0) function = "HID demo";
            else if (strcmp(name, "AppleUSBNCMData") == 0 && strcmp(function, "unknown") == 0)
                function = "NCM network sharing";
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return function;
}

static void host_state(const char *port, char *link, size_t link_size,
                       char *state, size_t state_size, char *speed, size_t speed_size)
{
    snprintf(link, link_size, "unknown");
    snprintf(state, state_size, "Unknown");
    snprintf(speed, speed_size, "Unknown");
    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOUSBDeviceController"), &iter) != KERN_SUCCESS) return;
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        io_string_t path;
        if (IORegistryEntryGetPath(service, kIOServicePlane, path) == KERN_SUCCESS &&
                strstr(path, port)) {
            CFDictionaryRef current = IORegistryEntryCreateCFProperty(service,
                CFSTR("CurrentState"), kCFAllocatorDefault, 0);
            if (current && CFGetTypeID(current) == CFDictionaryGetTypeID()) {
                CFTypeRef on_bus = CFDictionaryGetValue(current, CFSTR("OnBus"));
                if (on_bus && CFGetTypeID(on_bus) == CFBooleanGetTypeID()) {
                    snprintf(link, link_size, "%s",
                        CFBooleanGetValue((CFBooleanRef)on_bus) ? "connected" : "disconnected");
                }
                CFTypeRef device_state = CFDictionaryGetValue(current, CFSTR("DeviceState"));
                if (device_state && CFGetTypeID(device_state) == CFStringGetTypeID())
                    CFStringGetCString((CFStringRef)device_state, state, state_size, kCFStringEncodingUTF8);
                CFTypeRef connection_speed = CFDictionaryGetValue(current, CFSTR("ConnectionSpeedDescription"));
                if (connection_speed && CFGetTypeID(connection_speed) == CFStringGetTypeID())
                    CFStringGetCString((CFStringRef)connection_speed, speed, speed_size, kCFStringEncodingUTF8);
            }
            if (current) CFRelease(current);
            IOObjectRelease(service);
            break;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
}

int list_usb_ports(void)
{
    io_iterator_t iter = 0;
    kern_return_t ret = IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("AppleARMIODevice"), &iter);
    if (ret != KERN_SUCCESS) {
        fprintf(stderr, "Could not enumerate USB device controllers: 0x%x\n", ret);
        return 1;
    }
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        io_string_t path;
        io_name_t name;
        if (IORegistryEntryGetPath(service, kIOServicePlane, path) == KERN_SUCCESS &&
                IORegistryEntryGetName(service, name) == KERN_SUCCESS && valid_usb_port(name)) {
            char link[16], state[64], speed[64];
            host_state(name, link, sizeof(link), state, sizeof(state), speed, sizeof(speed));
            printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", name,
                gadget_present(name) ? "ready" : "kext missing",
                current_function(name), link, state, speed, path);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return 0;
}
