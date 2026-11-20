#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach.h>
//#include "IOUSBDeviceControllerLib.h"
#include "alt_IOUSBDeviceControllerLib.h"

#include "usb_hid_keys.h"
#include "ports.h"

CFStringRef serial = CFSTR("Look, Custom Serial");

#define USB_EP_TYPE_CTRL (0)
#define USB_EP_TYPE_ISOC (1)
#define USB_EP_TYPE_BULK (2)
#define USB_EP_TYPE_INT  (3)

#define USB_EP_DIR_OUT (0)
#define USB_EP_DIR_IN (1)

const char* some_str = "keyboard_idk";

volatile sig_atomic_t stop;

static const char *selected_port;
static unsigned char message[1024];
static size_t message_len;
static int live_mode;

static int hid_key_for_ascii(unsigned char c, uint8_t *key, uint8_t *modifier)
{
    *modifier = 0;
    if (c >= 'a' && c <= 'z') { *key = KEY_A + c - 'a'; return 1; }
    if (c >= 'A' && c <= 'Z') { *key = KEY_A + c - 'A'; *modifier = 2; return 1; }
    if (c >= '1' && c <= '9') { *key = KEY_1 + c - '1'; return 1; }
    if (c == '0') { *key = KEY_0; return 1; }
    if (c == ' ') { *key = KEY_SPACE; return 1; }
    if (c == '\n' || c == '\r') { *key = KEY_ENTER; return 1; }
    if (c == '\t') { *key = KEY_TAB; return 1; }
    if (c == '\b' || c == 0x7f) { *key = KEY_BACKSPACE; return 1; }
    if (c == 0x1b) { *key = KEY_ESC; return 1; }
    const char *plain = "-=[]\\;',./`";
    const char *shifted = "_+{}|:\"<>?~";
    const uint8_t keys[] = {KEY_MINUS, KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE,
        KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_COMMA, KEY_DOT,
        KEY_SLASH, KEY_GRAVE};
    for (size_t i = 0; i < sizeof(keys); i++) {
        if (c == plain[i] || c == shifted[i]) {
            *key = keys[i];
            *modifier = c == shifted[i] ? 2 : 0;
            return 1;
        }
    }
    const char *shifted_digits = "!@#$%^&*()";
    for (size_t i = 0; i < 10; i++) {
        if (c == shifted_digits[i]) {
            *key = i == 9 ? KEY_0 : KEY_1 + i;
            *modifier = 2;
            return 1;
        }
    }
    return 0;
}

static int read_message(void)
{
    int ch;
    while ((ch = getchar()) != EOF) {
        if (message_len == sizeof(message)) {
            fprintf(stderr, "Text is too long (maximum 1024 bytes).\n");
            return 1;
        }
        uint8_t key, modifier;
        if (!hid_key_for_ascii((unsigned char)ch, &key, &modifier)) {
            fprintf(stderr, "Unsupported character at byte %zu (0x%02x); use US ASCII text.\n",
                message_len + 1, (unsigned char)ch);
            return 1;
        }
        message[message_len++] = (unsigned char)ch;
    }
    if (ferror(stdin) || message_len == 0) {
        fprintf(stderr, "No text was provided on standard input.\n");
        return 1;
    }
    return 0;
}

static int read_live_byte(void)
{
    unsigned char byte;
    ssize_t count;
    do {
        count = read(STDIN_FILENO, &byte, 1);
    } while (count < 0 && errno == EINTR && !stop);
    return count == 1 ? byte : EOF;
}

static kern_return_t write_hid_report(io_connect_t data_port, void *buffer,
                                      uint64_t pipe_id, uint64_t map_token,
                                      const uint8_t report[8])
{
    uint64_t args[4] = {pipe_id, map_token, 8, 100};
    uint64_t output = 0;
    uint32_t output_count = 1;
    memset(buffer, 0, 16);
    memcpy(buffer, report, 8);
    return IOConnectCallScalarMethod(data_port, 14, args, 4, &output, &output_count);
}

void inthand(int signum) {
    stop = 1;
}

int make_a_keyboard()
{
    // TODO: A lot more error checking
    io_iterator_t       iter    = 0;
    io_service_t service = 0;
    io_service_t service_kext = 0;
    kern_return_t s_ret;
    IOReturn ret;

    io_connect_t dataPort = 0;
    io_connect_t dataPort_kext;
    kern_return_t open_ret;

    uint64_t output[10] = {0};
    uint32_t outputCount = 0;

    uint64_t args[10] = { 0, 0, 0, 0, 0, 0,0,0,0,0 };

    alt_IOUSBDeviceControllerRef controller;

    //IOUSBDeviceControllerSendCommand(controller, CFSTR("ShowDeviceDebug"), NULL); 

    for (int i = 0; i < 5; i++)
    {
        CFMutableDictionaryRef match = IOServiceMatching("gay_bowser_usbgadget");
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        //CFDictionarySetValue(dict, CFSTR("USBDeviceFunction"), CFSTR("MyUSBData"));
        CFDictionarySetValue(match, CFSTR("IOPropertyMatch"), dict);

        CFRetain(match);
        ret = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
        if (ret != KERN_SUCCESS || iter == 0) {
            printf("No matching usbgadget service (%x)...\n", ret);
            sleep(1);
            continue;
        }

        // Get the third port
        while (1)
        {
            service_kext = IOIteratorNext(iter);
            if (!service_kext) break;

            io_string_t path;
            if (IORegistryEntryGetPath(service_kext, kIOServicePlane, path) != KERN_SUCCESS) {
                IOObjectRelease(service_kext);
                continue;
            }

            if (strstr(path, selected_port)) {
                printf("Connecting to: '%s'\n", path);
                break;
            }

            IOObjectRelease(service_kext);
        } 

        CFRelease(match);
        IOObjectRelease(iter);

        if (service_kext) break;
    }

    if (!service_kext) {
        fprintf(stderr, "usbgadget.kext is not loaded or did not match %s; aborting.\n", selected_port);
        return 1;
    }

    //open_ret = IOServiceOpen(service_kext, mach_task_self(), 123, &dataPort_kext);
    //printf("open_ret kext %x %x\n", open_ret, dataPort_kext);

    ret = alt_IOUSBDeviceControllerCreateFromService(kCFAllocatorDefault, service_kext, &controller);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "Failed to create USB device controller (%x); aborting.\n", ret);
        IOObjectRelease(service_kext);
        return 1;
    }
    //int configId = 0;
#if 1
    alt_IOUSBDeviceDescriptionRef desc = alt_IOUSBDeviceDescriptionCreate(kCFAllocatorDefault);//alt_IOUSBDeviceDescriptionCreateFromController(kCFAllocatorDefault, controller);
    if (!desc) {
        fprintf(stderr, "Failed to create USB device description; aborting.\n");
        IOObjectRelease(service_kext);
        alt_IOUSBDeviceControllerRelease(controller);
        CFAllocatorDeallocate(kCFAllocatorDefault, controller);
        return 1;
    }
    alt_IOUSBDeviceDescriptionSetSerialString(desc, serial);

    alt_IOUSBDeviceDescriptionSetProductID(desc, 0x1234);

    alt_IOUSBDeviceDescriptionRemoveAllConfigurations(desc);

    int configId = alt_IOUSBDeviceDescriptionAppendConfiguration(desc, CFSTR("JustAKeyboard"), 0, 0);
    alt_IOUSBDeviceDescriptionAppendInterfaceToConfiguration(desc, configId, CFSTR("AppleUSBMux"));
    alt_IOUSBDeviceDescriptionAppendInterfaceToConfiguration(desc, configId, CFSTR("MyUSBData"));
    //alt_IOUSBDeviceDescriptionAppendInterfaceToConfiguration(desc, configId, CFSTR("MyUSBControlAux"));
    //alt_IOUSBDeviceDescriptionAppendInterfaceToConfiguration(desc, configId, CFSTR("MyUSBData"));
    //alt_IOUSBDeviceDescriptionAppendInterfaceToConfiguration(desc, configId, CFSTR("MyUSBDataAux"));
    //CFDictionarySetValue(desc->info, , kCFBooleanTrue);
#endif

    ret = alt_IOUSBDeviceControllerSetDescription(controller, desc);
    printf("%x: system=%x subsystem=%x code=%x\n", ret, err_get_system(ret), err_get_sub(ret), err_get_code(ret));
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "Failed to set USB device description; aborting.\n");
        IOObjectRelease(service_kext);
        alt_IOUSBDeviceControllerRelease(controller);
        CFAllocatorDeallocate(kCFAllocatorDefault, controller);
        alt_IOUSBDeviceDescriptionRelease(desc);
        CFAllocatorDeallocate(kCFAllocatorDefault, desc);
        return 1;
    }

    //printf("%x\n", IORegistryEntrySetCFProperty(service_kext, CFSTR("AAAA"), CFSTR("BBBB")));

    //IOUSBDeviceControllerSetPreferredConfiguration(controller, configId);

    printf("%x\n", IORegistryEntrySetCFProperty(service_kext, CFSTR("Poke"), CFSTR("Poke")));


    // Poke usbgadget
    //outputCount = 0;
    //s_ret = IOConnectCallScalarMethod(dataPort_kext, 0, args, 0, output, &outputCount);
    //printf("poke s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    //IOServiceClose(dataPort_kext);
    IOObjectRelease(service_kext);

    alt_IOUSBDeviceControllerRelease(controller);
    CFAllocatorDeallocate(kCFAllocatorDefault, controller);

    alt_IOUSBDeviceDescriptionRelease(desc);
    CFAllocatorDeallocate(kCFAllocatorDefault, desc);

    // Wait for the old interface to get nuked
    sleep(1);

    for (int i = 0; i < 10; i++)
    {
        CFMutableDictionaryRef match = IOServiceMatching("IOUSBDeviceInterface");
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(dict, CFSTR("USBDeviceFunction"), CFSTR("MyUSBData"));
        CFDictionarySetValue(match, CFSTR("IOPropertyMatch"), dict);

        CFRetain(match);
        ret = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
        if (ret != KERN_SUCCESS || iter == 0) {
            printf("Error matching IOUSBDeviceInterface (%x)...\n", ret);
            sleep(1);
            continue;
        }

        // Get the third port
        while (1)
        {
            service = IOIteratorNext(iter);
            if (!service) break;

            io_string_t path;
            if (IORegistryEntryGetPath(service, kIOServicePlane, path) != KERN_SUCCESS) {
                IOObjectRelease(service);
                continue;
            }

            if (strstr(path, selected_port)) {
                printf("Connecting to: '%s'\n", path);
                break;
            }
            IOObjectRelease(service);
        } 

        CFRelease(match);
        IOObjectRelease(iter);

        if (service) break;
    }

    if (!service) {
        printf("Failed to find IOUSBDeviceInterface, aborting...\n");
        return 1;
    }

    open_ret = IOServiceOpen(service, mach_task_self(), 123, &dataPort);
    printf("open_ret %x %x\n", open_ret, dataPort);
    //IOServiceClose(service);
    IOObjectRelease(service);
    if (open_ret != KERN_SUCCESS) {
        fprintf(stderr, "Failed to open USB device interface; aborting.\n");
        return 1;
    }

    
    // Open
    outputCount = 0;
    s_ret = IOConnectCallScalarMethod(dataPort, 0, args, 1, output, &outputCount);
    printf("open s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    // SetClass
    outputCount = 0;
    args[0] = 3; // HID
    args[1] = configId;
    s_ret = IOConnectCallScalarMethod(dataPort, 3, args, 2, output, &outputCount);
    printf("setclass s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    // SetSubclass
    outputCount = 0;
    args[0] = 1; // boot if
    args[1] = configId;
    s_ret = IOConnectCallScalarMethod(dataPort, 4, args, 2, output, &outputCount);
    printf("setsubclass s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    // SetProtocol
    outputCount = 0;
    args[0] = 1; // keyboard
    args[1] = configId;
    s_ret = IOConnectCallScalarMethod(dataPort, 5, args, 2, output, &outputCount);
    printf("setprotocol s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

#if 0
    mach_port_t wakePort;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &wakePort);

    args[0] = 0;
    args[1] = (uint64_t) callback_2;
    args[2] = (uint64_t) 0; // refcon
    IOConnectCallAsyncMethod(dataPort, 8, wakePort, args, 3, NULL, 0, NULL, 0, NULL, 0, NULL, NULL);
    printf("async s_ret %x\n", s_ret, outputCount);
#endif

#if 0
    // CreatePipe
    outputCount = 1;
    output[0] = 0;
    args[0] = USB_EP_TYPE_BULK;
    args[1] = USB_EP_DIR_OUT;
    args[2] = -1;
    args[3] = 0;
    args[4] = 0;
    args[5] = configId;
    s_ret = IOConnectCallScalarMethod(dataPort, 10, args, 6, output, &outputCount);
    printf("s_ret %x %x (%x %x)\n", s_ret, outputCount, output[0], output[1]);

    // CreatePipe
    outputCount = 1;
    output[0] = 0;
    args[0] = USB_EP_TYPE_BULK; // type
    args[1] = USB_EP_DIR_IN; // direction
    args[2] = -1;
    args[3] = 0xff; // interval
    args[4] = 0;
    args[5] = configId;
    s_ret = IOConnectCallScalarMethod(dataPort, 10, args, 6, output, &outputCount);
    printf("s_ret %x %x (%x %x)\n", s_ret, outputCount, output[0], output[1]);

    // CreatePipe
    outputCount = 1;
    output[0] = 0;
    args[0] = USB_EP_TYPE_INT; // type
    args[1] = USB_EP_DIR_OUT; // direction
    args[2] = -1;
    args[3] = 0;
    args[4] = 0;
    args[5] = configId;
    s_ret = IOConnectCallScalarMethod(dataPort, 10, args, 6, output, &outputCount);
    printf("s_ret %x %x (%x %x)\n", s_ret, outputCount, output[0], output[1]);
#endif

    uint8_t hid_desc[] = {0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x5D, 0x00};
    uint8_t hid_report_desc[] = { 0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x03, 0x75, 0x01,
  0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x91, 0x02, 0x95, 0x05, 0x75, 0x01, 0x91, 0x01, 0x95, 0x06,
  0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,
  0xC0};

    // setDesc
    /*outputCount = 0;
    output[0] = some_str;
    args[0] = some_str; // idk
    s_ret = IOConnectCallMethod(dataPort, 2, NULL, 0, args, 1, NULL, NULL, NULL, NULL);
    printf("setdesc s_ret %x %x (%x %x)\n", s_ret, outputCount, output[0], output[1]);*/

    // appendStandardClassOrVendorDescriptor
    // This gets added after the interface and before endpoints
    outputCount = 0;
    args[0] = 0x21; // descriptor type
    args[1] = 0xF;
    s_ret = IOConnectCallMethod(dataPort, 6, args, 2, hid_desc, sizeof(hid_desc), output, &outputCount, NULL, NULL);
    printf("desc s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    // appendNonstandardClassOrVendorDescriptor
    // this has to be asked for manually via the ctrl ep
    outputCount = 0;
    args[0] = 0x22; // class descriptor type
    args[1] = 0xF;
    s_ret = IOConnectCallMethod(dataPort, 7, args, 2, hid_report_desc, sizeof(hid_report_desc), output, &outputCount, NULL, NULL);
    printf("append s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    // CreatePipe
    outputCount = 1;
    output[0] = 0;
    args[0] = USB_EP_TYPE_INT; // type
    args[1] = USB_EP_DIR_IN; // direction
    args[2] = 8;
    args[3] = 10;
    args[4] = 0;
    args[5] = configId;
    s_ret = IOConnectCallScalarMethod(dataPort, 10, args, 6, output, &outputCount);
    printf("createPipe s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    uint64_t pipe_id = output[0];

    // CommitConfiguration
    outputCount = 0;
    s_ret = IOConnectCallScalarMethod(dataPort, 11, args, 0, output, &outputCount);
    printf("CommitConfiguration s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    // createData
    outputCount = 3;
    args[0] = 0x1000;
    s_ret = IOConnectCallScalarMethod(dataPort, 18, args, 1, output, &outputCount);
    printf("createData s_ret %x %x (%llx %llx %llx)\n", s_ret, outputCount, output[0], output[1], output[2]);
    if (s_ret != KERN_SUCCESS || outputCount < 3 || output[0] == 0) {
        fprintf(stderr, "Failed to allocate USB report buffer; aborting.\n");
        IOServiceClose(dataPort);
        return 1;
    }

    void* dataptr = (void*)output[0];
    uint64_t data_cap = output[1];
    uint64_t map_token = output[2];

    // Force the device to re-enumerate
    //IOUSBDeviceControllerGoOffAndOnBus(controller, 1000);

#if 0
    // MapBuffer
    outputCount = 3;
    args[0] = 0;
    args[1] = 0;
    s_ret = IOConnectCallScalarMethod(dataPort, 26, args, 2, output, &outputCount);
    printf("map s_ret %x %x (%llx %llx %llx)\n", s_ret, outputCount, output[0], output[1], output[2]);
#endif

    int send_failed = 0;
    size_t sent_count = 0;
    uint8_t held_report[8] = {0};
    int holding = 0;
    kern_return_t last_hold_error = KERN_SUCCESS;
    if (live_mode) {
        puts("READY");
        fflush(stdout);
    }
    while (!stop) {
        unsigned char character;
        if (live_mode) {
            struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
            int ready = poll(&input, 1, holding ? 100 : -1);
            if (ready < 0) {
                if (errno == EINTR) continue;
                perror("poll");
                send_failed = 1;
                break;
            }
            if (ready == 0) {
                s_ret = write_hid_report(dataPort, dataptr, pipe_id, map_token, held_report);
                if (s_ret != last_hold_error) {
                    if (s_ret == KERN_SUCCESS) puts("Held key resumed after USB reconnection.");
                    else fprintf(stderr, "Held key is waiting for USB host (0x%x).\n", s_ret);
                    fflush(stdout);
                    last_hold_error = s_ret;
                }
                continue;
            }
            int next = read_live_byte();
            if (next == EOF) break;
            if (next == 1) {
                int key = read_live_byte();
                int modifier = read_live_byte();
                if (key == EOF || modifier == EOF) {
                    fprintf(stderr, "Incomplete hold command.\n");
                    send_failed = 1;
                    break;
                }
                memset(held_report, 0, sizeof(held_report));
                held_report[0] = (uint8_t)modifier;
                held_report[2] = (uint8_t)key;
                holding = 1;
                s_ret = write_hid_report(dataPort, dataptr, pipe_id, map_token, held_report);
                last_hold_error = s_ret;
                if (s_ret != KERN_SUCCESS)
                    fprintf(stderr, "Held key is waiting for USB host (0x%x).\n", s_ret);
                printf("Holding HID key 0x%02x with modifier 0x%02x.\n", key, modifier);
                fflush(stdout);
                continue;
            }
            if (next == 2) {
                memset(held_report, 0, sizeof(held_report));
                holding = 0;
                s_ret = write_hid_report(dataPort, dataptr, pipe_id, map_token, held_report);
                if (s_ret != KERN_SUCCESS)
                    fprintf(stderr, "Host was disconnected when held key was released (0x%x).\n", s_ret);
                last_hold_error = KERN_SUCCESS;
                puts("Released held key.");
                fflush(stdout);
                continue;
            }
            if (holding) {
                fprintf(stderr, "Release the held key before typing text.\n");
                fflush(stderr);
                continue;
            }
            character = (unsigned char)next;
        } else {
            if (sent_count >= message_len) break;
            character = message[sent_count];
        }
        uint8_t key, modifier;
        if (!hid_key_for_ascii(character, &key, &modifier)) {
            fprintf(stderr, "Unsupported live character: 0x%02x\n", character);
            send_failed = 1;
            break;
        }
        uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
        int attempts = 0;
        do {
            s_ret = write_hid_report(dataPort, dataptr, pipe_id, map_token, report);
            if (s_ret == 0xE0000001 && attempts++ < 10) sleep(1);
            else break;
        } while (!stop);
        if (s_ret != KERN_SUCCESS) {
            fprintf(stderr, "Key %zu could not be sent: 0x%x\n", sent_count + 1, s_ret);
            send_failed = 1;
            break;
        }
        uint8_t released[8] = {0};
        s_ret = write_hid_report(dataPort, dataptr, pipe_id, map_token, released);
        if (s_ret != KERN_SUCCESS) {
            fprintf(stderr, "Key %zu could not be released: 0x%x\n", sent_count + 1, s_ret);
            send_failed = 1;
            break;
        }
        sent_count++;
    }

    if (holding) {
        uint8_t released[8] = {0};
        write_hid_report(dataPort, dataptr, pipe_id, map_token, released);
    }

    // ReleaseBuffer
    outputCount = 0;
    args[0] = map_token;
    s_ret = IOConnectCallScalarMethod(dataPort, 19, args, 1, output, &outputCount);
    printf("release s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    // Close
    outputCount = 0;
    output[0] = 0;
    output[1] = 0;
    s_ret = IOConnectCallScalarMethod(dataPort, 1, args, 0, output, &outputCount);
    printf("close s_ret %x %x (%llx %llx)\n", s_ret, outputCount, output[0], output[1]);

    IOServiceClose(dataPort);

    //mach_port_mod_refs(mach_task_self(), wakePort, MACH_PORT_RIGHT_RECEIVE, -1);
    if (stop) return 130;
    if (send_failed) return 1;
    printf("Submitted %zu key events to %s. Verify delivery on the receiving computer.\n",
        sent_count, selected_port);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, inthand);
    if (argc == 2 && strcmp(argv[1], "--list") == 0) return list_usb_ports();
    if (argc == 4 && strcmp(argv[1], "--live") == 0 &&
            strcmp(argv[2], "--port") == 0 && valid_usb_port(argv[3])) {
        live_mode = 1;
        selected_port = argv[3];
        return make_a_keyboard();
    }
    if (argc != 3 || strcmp(argv[1], "--port") != 0 || !valid_usb_port(argv[2])) {
        fprintf(stderr, "Usage: %s --list | --port usb-drdN < text.txt | --live --port usb-drdN\n", argv[0]);
        return 2;
    }
    selected_port = argv[2];
    if (read_message()) return 2;
    return make_a_keyboard();
}
