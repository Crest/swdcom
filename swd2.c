// pkg install -y stlink
// build with: cc -O0 -g -std=c99 -Wextra -I/usr/local/include -L/usr/local/lib -o swd2 swd2.c -lstlink-shared
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <stlink.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <time.h>
#include <sys/endian.h>
#include <poll.h>
#include <sys/stat.h>

// An evil collection of global variables follows.
static uint32_t addr = 0; // the base address of the ring buffers
static stlink_t *handle = NULL; // handle to the ST/LINK V2
static struct termios orig[1]; // original terminal settings

static bool quit       = false; // Quit the main loop
static bool stdin_tty  = false; // Is stdin a TTY?
static bool stdin_pipe = false; // Is stdin a pipe?
static bool stdin_file = false; // Is stdin a regular file?

// On TTYs ctrl+d results in a ASCII end of transmission control character.
static const uint8_t ascii_eot = 0x04;

// Close the the ST/LINK V2 correctly.
static void
close_handle(void)
{
	if ( handle ) {
		fprintf(stderr, "Closing ST-LINK/V2 handle.\n");
		stlink_close(handle);
		handle = NULL;
	}
}

// Open the first ST/LINK V2 connected via USB
// Register an atexit() handler to close it.
static stlink_t *
open_or_die(void)
{
	enum ugly_loglevel loglevel = UWARN;
	bool reset = false;
	handle = stlink_open_usb(loglevel, reset, NULL);
	if ( !handle ) {
		fprintf(stderr, "Failed to open the debugger.\n");
		abort();
	}
	atexit(close_handle);

	if ( stlink_set_swdclk(handle, STLINK_SWDCLK_4MHZ_DIVISOR) ) {
		fprintf(stderr, "WARNING: Failed to set SWD clock rate to 4MHz.\n");
	}

	return handle;
}

// Set the ring buffer base address.
static void 
set_addr_or_die(const char *hex_addr)
{
	errno = 0;
	long result = strtol(hex_addr, NULL, 16);
	if ( errno ) {
		fprintf(stderr, "Failed to convert base address: %s.\n", strerror(errno));
		abort();
	}
	addr = (uint32_t)result;
}

// Read the ring buffer indicies from the microcontroller.
// All four indicies ({R, W} x {TX, RX}) are stored in a naturally aligned 32 bit word.
static uint32_t
read_indicies_or_die(void)
{
	if ( stlink_read_mem32(handle, addr, 4) ) {
		fprintf(stderr, "Failed to read the ringbuffer indicies.\n");
		abort();
	}
	return le32toh(*((uint32_t*)handle->q_buf));
}

// Retry interrupted or blocked writes. Abort on I/O error.
static void
write_or_die(const void *buffer, size_t len)
{
	while ( len > 0 ) {
		ssize_t delta = write(STDOUT_FILENO, buffer, len);
		if ( delta < 0 ) {
			if ( errno == EAGAIN || errno == EINTR ) {
				continue;
			} else {
				fprintf(stderr, "Failed to write to stdout.\n");
				abort();
			}
		}
		len -= delta;
		buffer += delta;
	}
}

// Write a buffer to the microcontroller 8 bit at a time. Abort on I/O error.
// Using write8_or_die() is slower than write32_or_die().
static void
write8_or_die(uint32_t destination, const void *source, size_t length_in_bytes)
{
	const size_t buffer_size = Q_BUF_LEN;
	if ( length_in_bytes > buffer_size ) {
		fprintf(stderr, "Oversized bytewise write failed (length = %zu, max = %zu).\n",
				length_in_bytes, buffer_size);
		abort();
	}
	memcpy(handle->q_buf, source, length_in_bytes);
	if ( stlink_write_mem8(handle, destination, length_in_bytes) ) {
		fprintf(stderr, "Bytewise write failed (destination = 0x%08x, length = %zu).\n",
				destination, length_in_bytes);
		abort();
	}
}

// Write a buffer to the microcontroll 32 bit at at time. Abort on I/O error.
// Length has to be a multiple of four.
// Using write32_or_die() is faster than write8_or_die().
static void
write32_or_die(uint32_t destination, const void *source, size_t length_in_bytes)
{
	const size_t buffer_size = Q_BUF_LEN;
	if ( length_in_bytes > buffer_size ) {
		fprintf(stderr, "Oversized wordwise write failed (length = %zu, max = %zu).\n",
				length_in_bytes, buffer_size);
		abort();
	}
	memcpy(handle->q_buf, source, length_in_bytes);
	if ( stlink_write_mem32(handle, destination, length_in_bytes) ) {
		fprintf(stderr, "Wordwise write failed (destination = 0x%08x, length = %zu).\n",
				destination, length_in_bytes);
		abort();
	}
}

// Consume everything enqueued by the microcontroller into the ringbuffers.
// The ring buffers are single producer single writer queues.
static bool
consume(uint32_t indicies)
{
	uint8_t rx_w = (uint8_t)(indicies >> 16);
	uint8_t rx_r = (uint8_t)(indicies >> 24);
	uint8_t rx_u = rx_w - rx_r;
	
	if ( !rx_u ) {
		return false;
	}

	const uint32_t start  = rx_r;
	const uint32_t off    = start & 3;
	const uint16_t len    = rx_w > rx_r ? rx_u : (uint8_t)-rx_r;
	const uint32_t start0 = start & -4;
	const uint32_t start1 = 0;
	const uint16_t len0   = (len + off + 3) & -4;
	const uint16_t len1   = (rx_u + 3 - len) & -4;

	uint32_t count = 0;
	if ( len0 ) {
		stlink_read_mem32(handle, addr + 4 + 256 + start0, len0);
		write_or_die(handle->q_buf + off, len);
	}
	if ( len1 ) {
		stlink_read_mem32(handle, addr + 4 + 256 + start1, len1);
		write_or_die(handle->q_buf, rx_u - len);
	}

	handle->q_buf[0] = rx_w;
	if ( stlink_write_mem8(handle, addr + 3, 1) ) {
		fprintf(stderr, "Failed to advance RX read index.\n");
		abort();
	}

	return true;
}

// Attempt to read and enqueue as much input as possible from stdin to the microcontroller.
// The ring buffers are single producer single writer queues.
static bool
produce(uint32_t indicies)
{
	uint8_t tx_w = (uint8_t)(indicies >> 0);
	uint8_t tx_r = (uint8_t)(indicies >> 8);
	uint8_t tx_f = 255 - (tx_w - tx_r);

	uint8_t buffer[256];
	uint8_t count = 0;

	if ( !tx_f ) {
		return false;
	}
	
	struct pollfd fds[1] = {{
		.fd      = STDIN_FILENO,
		.events  = POLLIN | POLLOUT,
		.revents = 0
	}};
	int ready = 0;

	// We have to poll() the non-blocking pipe to differentiate between
	// a closed writer and an empty buffer.
retry:
	if ( stdin_pipe && (ready = poll(fds, 1, 0)) == -1 ) {
		if ( errno == EINTR ) {
			goto retry;
		}
		fprintf(stderr, "Failed to poll stdin: %s.\n", strerror(errno));
		abort();
	}

	errno = 0;
	ssize_t result = read(STDIN_FILENO, buffer, tx_f);
	if ( errno == EAGAIN || !result ) {
		fprintf(stderr, "read(stdin) -> %zi, err = %s\n", result, strerror(errno));
	}

	if ( result < 0 ) {
		if ( errno != EINTR && errno != EAGAIN ) {
			fprintf(stderr, "Failed to read from stdin: %s.\n", strerror(errno));	
			abort();
		} else {
			return false;
		}
	}
	count = (uint8_t)result;

	// On TTYs EOF is signaled with a ASCII end of transmission control character.
	// Scan for EO
	if ( stdin_tty ) {
		const uint8_t *eof = memchr(buffer, ascii_eot, count);
		if ( eof ) {
			count = (uint8_t)(eof - buffer); 
			quit = true;
		}
	}
	if ( stdin_pipe && fds[0].revents & POLLHUP ) {
		quit = true;
	}
	if ( stdin_file && !count ) {
		quit = true;
	}

	// Optimize the writes:
	// * The buffer is word aligned
	// * Start with 8 bit writes if necessary until 32 bit alignment is reached
	// * Use as many 32 bit where possible
	// * Finish with 8 bit writes if necessary
	// * The ring buffer can wrap around
	bool carry = (tx_w + count) >> 8;
	uint8_t len1 = carry ? tx_w + count : 0;
	uint8_t len0 = count - len1;
	uint8_t byte0 = (-tx_w & 3) > len0 ? len0 : -tx_w & 3;
	uint8_t byte1 = (len0 - byte0) % 4;
	uint8_t byte2 = len1 % 4;
	uint8_t word0 = len0 - byte0 - byte1;
	uint8_t word1 = len1 - byte2;

	uint8_t *input = buffer;
	uint32_t output = addr + 4 + tx_w;
	if ( byte0 ) {
		write8_or_die(output, input, byte0);
		input += byte0; output += byte0;
	}
	if ( word0 ) {
		write32_or_die(output, input, word0);
		input += word0; output += word0;
	}
	if ( byte1 ) {
		write8_or_die(output, input, byte1);
		input += byte1;
	}
	output = addr + 4;
	if ( word1 ) {
		write32_or_die(output, input, word1);
		input += word1; output += word1;
	}
	if ( byte2 ) {
		write8_or_die(output, input, byte2);
	}

	handle->q_buf[0] = tx_w + count;
	if ( stlink_write_mem8(handle, addr + 0, 1) ) {
		fprintf(stderr, "Failed to advance TX write index.\n");
		abort();
	}

	return true;
}

static void
restore_stdin(void)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, orig);
}

// Put the standard input into raw mode if it's a TTY. 
static void
raw_mode_or_die(void)
{
	if ( !stdin_tty ) {
		return;
	}

	if ( tcgetattr(STDIN_FILENO, orig) ) {
		abort();
	}
	atexit(restore_stdin);
	struct termios raw = *orig;
	raw.c_lflag &= ~(ECHO | ICANON);
	if ( tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) ) {
		fprintf(stderr, "Failed to put terminal into raw mode: %s.\n", strerror(errno));
		abort();
	}
}

static struct timespec
elapsed(struct timespec start, struct timespec stop)
{
	if ( stop.tv_nsec - start.tv_nsec < 0 ) {
		struct timespec result = {
			.tv_sec  = stop.tv_sec  - start.tv_sec  - 1,
			.tv_nsec = stop.tv_nsec - start.tv_nsec + 1000000000
		};
		return result;
	} else {
        	struct timespec result = {
			.tv_sec  = stop.tv_sec  - start.tv_sec,
        		.tv_nsec = stop.tv_nsec - start.tv_nsec
		};
		return result;
	}
}

static void
stdin_nonblock_or_die(void)
{
	errno = 0;
	int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	if ( errno ) {
		fprintf(stderr, "Failed to get file descriptor status flags: %s.\n", strerror(errno));
		abort();
	}
	if ( fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == -1 ) {
		fprintf(stderr, "Failed to add O_NONBLOCK to file descriptor status flags: %s.\n", strerror(errno));
		abort();
	}
}

static struct timespec
now_or_die(void)
{
	struct timespec ts;
	if ( clock_gettime(CLOCK_UPTIME_FAST, &ts) ) {
		fprintf(stderr, "Failed to read uptime clock: %s.\n", strerror(errno));
		abort();
	}

	return ts;
}

static void
debug_indicies(uint32_t indicies)
{
	uint8_t tx_w = (uint8_t)(indicies >>  0);
	uint8_t tx_r = (uint8_t)(indicies >>  8);
	uint8_t rx_w = (uint8_t)(indicies >> 16);
	uint8_t rx_r = (uint8_t)(indicies >> 24);
	uint8_t rx_u = rx_w - rx_r;
	uint8_t tx_f = 255 - (tx_w - tx_r);

	fprintf(stderr, "\nTX: r = %i, w = %i, f = %i RX: r = %i, w = %i, u = %i\n",
			tx_r, tx_w, tx_f, rx_r, rx_w, rx_u);
}

static void
stdin_file_type_or_die(void)
{
	struct stat sb;
	if ( fstat(STDIN_FILENO, &sb) ) {
		fprintf(stderr, "Failed to fstat() stdin: %s.\n", strerror(errno));
		abort();
	}
	const mode_t file_type = sb.st_mode & S_IFMT;
	switch ( file_type ) {
		case S_IFIFO:
			stdin_pipe = true;
			break;

		case S_IFCHR:
			stdin_tty = isatty(STDIN_FILENO);
			if ( !stdin_tty ) {
				fprintf(stderr, "TTYs are the only supported kind of character device.\n");
				abort();
			}
			break;
		case S_IFREG:
			stdin_file = true;
			break;
		default:
			fprintf(stderr, "unsupported file type: 0%o.\n", file_type );
			break;
	}
}

int
main(int argc, const char *argv[])
{
	if ( argc != 2 ) {
		fprintf(stderr, "usage: %s <base-addr>\n", argv[0]);
		return 1;
	}

	set_addr_or_die(argv[1]);
	stdin_file_type_or_die();
	open_or_die();
	raw_mode_or_die();
	stdin_nonblock_or_die();

	struct timespec last_active = now_or_die();
	while ( !quit ) {
		uint32_t indicies = read_indicies_or_die();
		bool rx_active = consume(indicies);
		bool tx_active = produce(indicies);
		bool active = rx_active | tx_active;
		struct timespec now = now_or_die();
		if ( clock_gettime(CLOCK_UPTIME_FAST, &now) ) {
			fprintf(stderr, "Failed to read uptime clock: %s.\n", strerror(errno));
			abort();
		}
		if ( active ) {
			last_active = now;
		} else {
			struct timespec diff = elapsed(last_active, now);
			if ( diff.tv_sec || diff.tv_nsec > 100000000 ) {
				usleep(10*1000);
			}
		}
	}

	return 0;
}
