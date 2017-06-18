#ifndef CUSTOM_IO_H
#define CUSTOM_IO_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

// Verbatim copy from src/serial.h

#ifndef __SERIAL_TYPES__
#define __SERIAL_TYPES__
// Don't re-declare when we're internal
/**
 * The parity checking scheme.
 */
typedef enum dc_parity_t {
	DC_PARITY_NONE, /**< No parity */
	DC_PARITY_ODD,  /**< Odd parity */
	DC_PARITY_EVEN, /**< Even parity */
	DC_PARITY_MARK, /**< Mark parity (always 1) */
	DC_PARITY_SPACE /**< Space parity (alwasy 0) */
} dc_parity_t;

/**
 * The number of stop bits.
 */
typedef enum dc_stopbits_t {
	DC_STOPBITS_ONE,          /**< 1 stop bit */
	DC_STOPBITS_ONEPOINTFIVE, /**< 1.5 stop bits*/
	DC_STOPBITS_TWO           /**< 2 stop bits */
} dc_stopbits_t;

/**
 * The flow control.
 */
typedef enum dc_flowcontrol_t {
	DC_FLOWCONTROL_NONE,     /**< No flow control */
	DC_FLOWCONTROL_HARDWARE, /**< Hardware (RTS/CTS) flow control */
	DC_FLOWCONTROL_SOFTWARE  /**< Software (XON/XOFF) flow control */
} dc_flowcontrol_t;

/**
 * The direction of the data transmission.
 */
typedef enum dc_direction_t {
	DC_DIRECTION_INPUT = 0x01,  /**< Input direction */
	DC_DIRECTION_OUTPUT = 0x02, /**< Output direction */
	DC_DIRECTION_ALL = DC_DIRECTION_INPUT | DC_DIRECTION_OUTPUT /**< All directions */
} dc_direction_t;

/**
 * The serial line signals.
 */
typedef enum dc_line_t {
	DC_LINE_DCD = 0x01, /**< Data carrier detect */
	DC_LINE_CTS = 0x02, /**< Clear to send */
	DC_LINE_DSR = 0x04, /**< Data set ready */
	DC_LINE_RNG = 0x08, /**< Ring indicator */
} dc_line_t;

#endif /* __SERIAL_TYPES__ */

struct dc_context_t;

typedef struct dc_custom_io_t
{
	void *userdata;

	// Custom serial (generally BT rfcomm)
	dc_status_t (*serial_open) (void **userdata, const char *name);
	dc_status_t (*serial_close) (void **userdata);
	dc_status_t (*serial_read) (void **userdata, void* data, size_t size, size_t *actual);
	dc_status_t (*serial_write) (void **userdata, const void* data, size_t size, size_t *actual);
	dc_status_t (*serial_purge) (void **userdata, dc_direction_t);
	dc_status_t (*serial_get_available) (void **userdata, size_t *value);
	dc_status_t (*serial_set_timeout) (void **userdata, long timeout);
	dc_status_t (*serial_configure) (void **userdata, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
	dc_status_t (*serial_set_dtr) (void **userdata, int level);
	dc_status_t (*serial_set_rts) (void **userdata, int level);
	dc_status_t (*serial_set_halfduplex) (void **userdata, unsigned int value);
	dc_status_t (*serial_set_break) (void **userdata, unsigned int level);
	//dc_serial_set_latency (dc_serial_t *device, unsigned int milliseconds) - Unused
	//dc_serial_get_lines (dc_serial_t *device, unsigned int *value) - Unused
	//dc_serial_flush (dc_serial_t *device) - No device interaction
	//dc_serial_sleep (dc_serial_t *device, unsigned int timeout) - No device interaction

	// Custom packet transfer (generally BLE GATT)
	int packet_size;
	dc_status_t (*packet_open) (struct dc_custom_io_t *, struct dc_context_t *, const char *);
	dc_status_t (*packet_close) (struct dc_custom_io_t *);
	dc_status_t (*packet_read) (struct dc_custom_io_t *, void* data, size_t size, size_t *actual);
	dc_status_t (*packet_write) (struct dc_custom_io_t *, const void* data, size_t size, size_t *actual);
} dc_custom_io_t;


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* CUSTOM_IO_H */
