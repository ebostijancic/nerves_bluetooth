/**
 * This module contains the functions to access the HCI devices
 * on the Kernel level.
 *
 * The functions originate as a port of Bleno's and Noble's
 * node-bluetooth-hci-socket project, providing access from node.js
 * to Bluetooth via HCI sockets.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <assert.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

// we do not include hci_lib.h, because the functions defined there are
// implemented in Elixir instead.

#include "global.h"
#include "hci_module.h"

#ifdef DEBUG
void vflog(const char *fmt, va_list args)  {
  FILE *f = fopen("./hci_ex.log", "a+");
  vfprintf(f, fmt, args);
  if (fmt[strlen(fmt) - 1] != '\n') {
    fprintf(f, "\n");
  }
  fclose(f);
}

void flog(const char *fmt, ...)  {
  va_list args;
  va_start(args, fmt);
  vflog(fmt, args);
  va_end(args);
}
#else

#endif


// TODO: Where does ATT_CID come from?
#define ATT_CID 4

/* =========================================*/

int _mode;
int hci_socket = -1;
int _dev_id;
uint8_t _address[6];
uint8_t _address_type;
// for handling the kernel problems 
int _l2cap_socket = -1;
int _l2cap_handle = -1;

/* =========================================*/


bool hci_init() {
  // SOCK__CLOEXEC enables event polling via epoll
  hci_socket = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
  LOG("Raw socket is: %d", hci_socket);
  return (hci_socket != -1);
}

int hci_close() {
  close(hci_socket);
  hci_socket = -1;
  return 0;
}

bool hci_is_dev_up() {
  LOG("enter hci_is_dev_up");
  struct hci_dev_info di;
  bool is_up = false;

  memset(&di, 0x00, sizeof(di));
  di.dev_id = _dev_id;

  if (ioctl(hci_socket, HCIGETDEVINFO, (void *)&di) > -1) {
    is_up = (di.flags & (1 << HCI_UP)) != 0;
  } else {
    int error = errno;
    LOG("ioctl returned <= -1, errno is set to %d", error); 
  }

  return is_up;
}

int hci_dev_id_for(int* p_dev_id, bool is_up) {
  LOG("enter hci_dev_id_for is_up=%d", is_up);
  int dev_id = -1; // default would be 0, but makes no sense to detect a dev id with a different state

  if (p_dev_id == NULL) {
    struct hci_dev_list_req *dl;
    struct hci_dev_req *dr;

    dl = (struct hci_dev_list_req*)calloc(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl), 1);
    dr = dl->dev_req;

    dl->dev_num = HCI_MAX_DEV;

    if (ioctl(hci_socket, HCIGETDEVLIST, dl) > -1) {
      for (int i = 0; i < dl->dev_num; i++, dr++) {
        bool dev_up = dr->dev_opt & (1 << HCI_UP);
        // bool match = is_up ? dev_up : !dev_up;
        bool match = (is_up == dev_up);

        if (match) {
          // choose the first device that is match
          // later on, it would be good to also HCIGETDEVINFO and check the HCI_RAW flag
          dev_id = dr->dev_id;
          LOG("Found matching dev_id %d", dev_id);
          break;
        }
      }
    }

    free(dl);
  } else {
    dev_id = *p_dev_id;
  }

  return dev_id;
}


int hci_bind_raw(int *dev_id) {
  struct sockaddr_hci a;
  struct hci_dev_info di;

  memset(&a, 0, sizeof(a));
  a.hci_family = AF_BLUETOOTH;
  a.hci_dev = hci_dev_id_for(dev_id, true);
  a.hci_channel = HCI_CHANNEL_RAW;

  _dev_id = a.hci_dev;
  _mode = HCI_CHANNEL_RAW;

  if (bind(hci_socket, (struct sockaddr *) &a, sizeof(a)) < 0) {
    int error = errno;
    LOG("Bind failed. Reason: %d (%s)", error, strerror(error));
    return -1;
  }

  // get the local address and address type
  memset(&di, 0x00, sizeof(di));
  di.dev_id = _dev_id;
  memset(_address, 0, sizeof(_address));
  _address_type = 0;

  if (ioctl(hci_socket, HCIGETDEVINFO, (void *)&di) > -1) {
    memcpy(_address, &di.bdaddr, sizeof(di.bdaddr));
    _address_type = di.type;

    if (_address_type == 3) {
      // 3 is a weird type, use 1 (public) instead
      _address_type = 1;
    }
  }

  return _dev_id;
}

int hci_write(byte *data, int size) {
  if (hci_socket < 0) {
    return ENOTCONN;
  }
  if (size < 1) {
    return 0;
  }
  
  // Set the event filter for answer, taken from cmd_cmd from hcitool.c
  struct hci_filter flt;
	hci_filter_clear(&flt);
	hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
	hci_filter_all_events(&flt);
	if (setsockopt(hci_socket, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
    int error = errno;
		LOG("HCI filter setup failed: %d (%s)", error, strerror(error));
		exit(-1);
	}
  
  int counter = 0;
  while (counter < size) {
    int result = write(hci_socket, data+counter, size - counter);
    if (result < 0) {
      if ((errno == EAGAIN || errno == EINTR)) 
        // try again
        continue;
      else { // failure
        hci_close(hci_socket);
        return errno;
      } 
    }
    else {
      counter += result;
    }
  }
  
  return 0;
}


int hci_set_filter(byte *data, int size) {
  struct hci_filter flt;
  if (size != sizeof(flt)) {
    LOG("size of filter data is %d instead of required %d. Aborting.", size, sizeof(flt));
    exit(-1);
  }

  if (setsockopt(hci_socket, SOL_HCI, HCI_FILTER, data, size) < 0) {
    int error = errno;
    LOG("setsockopts failed with: %d (%s)", error, strerror(error));
    return error;
  }
  return 0;
  /**
  
  */
}


/** 
 * I am not sure about this function. But it is part of the HCI_RAW_CHANNEL code in Noble and 
 * is always called after a positive read. Since my own version blocks after a positive read, 
 * it might be that Linux kernel has some problems and which are adressed here. 
 */
void kernelDisconnectWorkArounds(int length, char* data) {
  // HCI Event - LE Meta Event - LE Connection Complete => manually create L2CAP socket to force kernel to book keep
  // HCI Event - Disconn Complete =======================> close socket from above

  if (length == 22 && data[0] == 0x04 && data[1] == 0x3e && data[2] == 0x13 && data[3] == 0x01 && data[4] == 0x00) {
    int l2socket;
    struct sockaddr_l2 l2a;
    unsigned short l2cid;
    unsigned short handle = *((unsigned short*)(&data[5]));

#if __BYTE_ORDER == __LITTLE_ENDIAN
    l2cid = ATT_CID;
#elif __BYTE_ORDER == __BIG_ENDIAN
    l2cid = bswap_16(ATT_CID);
#else
    #error "Unknown byte order"
#endif

    l2socket = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);

    memset(&l2a, 0, sizeof(l2a));
    l2a.l2_family = AF_BLUETOOTH;
    l2a.l2_cid = l2cid;
    memcpy(&l2a.l2_bdaddr, _address, sizeof(l2a.l2_bdaddr));
    l2a.l2_bdaddr_type = _address_type;
    bind(l2socket, (struct sockaddr*)&l2a, sizeof(l2a));

    memset(&l2a, 0, sizeof(l2a));
    l2a.l2_family = AF_BLUETOOTH;
    memcpy(&l2a.l2_bdaddr, &data[9], sizeof(l2a.l2_bdaddr));
    l2a.l2_cid = l2cid;
    l2a.l2_bdaddr_type = data[8] + 1; // BDADDR_LE_PUBLIC (0x01), BDADDR_LE_RANDOM (0x02)

    connect(l2socket, (struct sockaddr *)&l2a, sizeof(l2a));

    // The original code has here a C++ map of handles to sockets. We allow only one here and see
    // what happens
    assert(_l2cap_socket == -1);
    _l2cap_socket = l2socket;
    _l2cap_handle = handle;
  } else if (length == 7 && data[0] == 0x04 && data[1] == 0x05 && data[2] == 0x04 && data[3] == 0x00) {
    unsigned short handle = *((unsigned short*)(&data[4]));

    assert(_l2cap_handle == handle);
    close(_l2cap_socket);
    _l2cap_handle = -1;
    _l2cap_socket = -1;
  }
}


/** Simple test function */
int hci_foo(int x) {
  return x+1;
}
