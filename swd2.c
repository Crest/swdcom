#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// CLOCK_MONOTONIC_FAST is FreeBSD specific.
// Fall back to CLOCK_MONOTONIC_COARSE if available.
// If neither is available use CLOCK_MONOTONIC which will likely be implemented as syscall.
#ifndef CLOCK_MONOTONIC_FAST
#ifdef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_FAST CLOCK_MONOTONIC_COARSE
#else
#define CLOCK_MONOTONIC_FAST CLOCK_MONOTONIC
#endif
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
#pragma clang diagnostic ignored "-Wpadded"
#include <stlink.h>
#pragma clang diagnostic pop

// An evil collection of global variables follows.
static uint32_t addr = 0; // the base address of the ring buffers
static stlink_t *handle = NULL; // handle to the ST/LINK V2
static struct termios orig[1]; // original terminal settings

static bool quit        = false; // Quit the main loop
static bool reset       = false; // Reset the target
static bool upload      = false; // Upload wanted
static bool new_file    = false; // Inject a file seperator?
static bool end_of_file = false; // Inject a end of medium?
static bool stdin_tty   = false; // Is stdin a TTY?
static bool stdin_pipe  = false; // Is stdin a pipe?
static bool stdin_file  = false; // Is stdin a regular file?
static int  fd          = STDIN_FILENO;
static int  line_num    = -1;

// On TTYs ctrl+d results in a ASCII end of transmission control character.
#define ASCII_EOT (0x04)
#define ASCII_ACK (0x06)
#define ASCII_NAK (0x15)
#define ASCII_CAN (0x18)
#define ASCII_EM  (0x19)
#define ASCII_FS  (0x1c)

static void
__attribute__((noreturn))
__attribute__((format(printf, 1, 2)))
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	vfprintf(stderr, fmt, ap);
#pragma clang diagnostic pop
	fputc('\n', stderr);
	exit(1);
	va_end(ap);
}

// Close the the ST/LINK V2 correctly.
static void
close_handle(void)
{
	if ( handle ) {
		stlink_close(handle);
		handle = NULL;
	}
}

// Open the first ST/LINK V2 connected via USB
// Register an atexit() handler to close it.
static stlink_t *
open_or_die(char serial[STLINK_SERIAL_MAX_SIZE])
{
	enum ugly_loglevel loglevel = UERROR;
	bool want_reset = false;
	handle = stlink_open_usb(loglevel, want_reset, serial, STLINK_SWDCLK_4MHZ_DIVISOR);
	if ( !handle ) {
		die("Failed to open the debugger.");
	}
	atexit(close_handle);

	return handle;
}

// Set the ring buffer base address.
static void
set_addr_or_die(const char *hex_addr)
{
	errno = 0;
	long result = strtol(hex_addr, NULL, 16);
	if ( errno ) {
		die("Failed to convert base address: %s.", strerror(errno));
	}
	addr = (uint32_t)result;
}

// Read the ring buffer indicies from the microcontroller.
// All four indicies ({R, W} x {TX, RX}) are stored in a naturally aligned 32 bit word.
static uint32_t
read_indicies_or_die(void)
{
	if ( stlink_read_mem32(handle, addr, 4) ) {
		die("Failed to read the ringbuffer indicies.");
	}
	return   ((uint32_t)handle->q_buf[0]) |
		(((uint32_t)handle->q_buf[1]) <<  8) |
		(((uint32_t)handle->q_buf[2]) << 16) |
		(((uint32_t)handle->q_buf[3]) << 24);
}

// Retry interrupted or blocked writes. Abort on I/O error.
static void
write_or_die(const void *buffer, size_t len)
{
	const uint8_t *pos = buffer;
	while ( len > 0 ) {
		ssize_t delta = write(STDOUT_FILENO, pos, len);
		if ( delta < 0 ) {
			if ( errno == EAGAIN || errno == EINTR ) {
				continue;
			} else {
				die("Failed to write to stdout.");
			}
		}
		len -= (size_t)delta;
		pos += delta;
	}
}

// Write a buffer to the microcontroller 8 bit at a time. Abort on I/O error.
// Using write8_or_die() is slower than write32_or_die().
static void
write8_or_die(uint32_t destination, const void *source, uint16_t length_in_bytes)
{
	const size_t buffer_size = Q_BUF_LEN;
	if ( length_in_bytes > buffer_size ) {
		die("Oversized bytewise write failed (length = %u, max = %zu).",
				length_in_bytes, buffer_size);
	}
	memcpy(handle->q_buf, source, length_in_bytes);
	if ( stlink_write_mem8(handle, destination, length_in_bytes) ) {
		die("Bytewise write failed (destination = 0x%08x, length = %u).",
				destination, length_in_bytes);
	}
}

// Write a buffer to the microcontroll 32 bit at at time. Abort on I/O error.
// Length has to be a multiple of four.
// Using write32_or_die() is faster than write8_or_die().
static void
write32_or_die(uint32_t destination, const void *source, uint16_t length_in_bytes)
{
	const size_t buffer_size = Q_BUF_LEN;
	if ( length_in_bytes > buffer_size ) {
		die("Oversized wordwise write failed (length = %u, max = %zu).",
				length_in_bytes, buffer_size);
	}
	memcpy(handle->q_buf, source, length_in_bytes);
	if ( stlink_write_mem32(handle, destination, length_in_bytes) ) {
		die("Wordwise write failed (destination = 0x%08x, length = %u).",
				destination, length_in_bytes);
	}
}

static void
end_upload(void)
{
	if ( fd != STDIN_FILENO ) {
		close(fd);
		fd = STDIN_FILENO;
	}
	if ( line_num >= 0 ) {
		end_of_file = true;
	}
}

static void
parse(uint8_t *reply, size_t len)
{
	for ( size_t i = 0; i < len; i++ ) {
		switch ( reply[i] ) {
			// Allow the target to disconnect from the host
			case ASCII_EOT:
				quit = true;
				break;

			// Send by QUIT after each line
			case ASCII_ACK:
				if ( line_num >= 0 ) {
					line_num++;
				}
				break;

			// Contained in all compiler errors
			case ASCII_NAK:
				if ( line_num >= 0 ) {
					fprintf(stderr, "\n*** Failure in line %i. ***\n", line_num);
				}
				end_upload();
				break;

			// Allow the target to cancel uploads
			case ASCII_CAN:
				end_upload();
				break;

			// Use end of medium as end of file marker
			case ASCII_EM:
				line_num = -1;
				break;

			// Begin each new file with file seperator marker
			case ASCII_FS:
				line_num = 0;
				break;
		}
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

	// Optimize reads:
	// * Use as few commands as possible
	// * Round down to 32 bit alignment
	// * Pad to 32 bit alignment
	// * The ring buffer can wrap around
	const uint32_t start  = rx_r;
	const uint32_t off    = start & 3;
	const uint16_t len    = rx_w > rx_r ? rx_u : (uint8_t)-rx_r;
	const uint32_t start0 = start & -4u;
	const uint32_t start1 = 0;
	const uint16_t len0   = (uint16_t)(len + off + 3) & -4u;
	const uint16_t len1   = (rx_u + 3 - len) & -4;

	if ( len0 ) {
		stlink_read_mem32(handle, addr + 4 + 256 + start0, len0);
		write_or_die(handle->q_buf + off, len);
		parse(handle->q_buf + off, len);
	}
	if ( len1 ) {
		stlink_read_mem32(handle, addr + 4 + 256 + start1, len1);
		write_or_die(handle->q_buf, rx_u - len);
		parse(handle->q_buf, rx_u - len);
	}

	handle->q_buf[0] = rx_w;
	if ( stlink_write_mem8(handle, addr + 3, 1) ) {
		die("Failed to advance RX read index.");
	}

	return true;
}

// Attempt to read and enqueue as much input as possible to the microcontroller.
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
	
	if ( new_file ) {
		const char helper[] = "\x1c\n$1c emit\n";
		if ( tx_f < strlen(helper) ) {
			return false;
		}
		memcpy(buffer, helper, strlen(helper));
		count = strlen(helper);
		new_file = false;
	} else if ( end_of_file ) {
		const char helper[] = "\x19\n$19 emit\n";
		if ( tx_f < strlen(helper) ) {
			return false;
		}
		memcpy(buffer, helper, strlen(helper));
		count = strlen(helper);
		end_of_file = false;
	} else {
		ssize_t result = read(fd, buffer, tx_f);
		if ( result < 0 ) {
			if ( errno != EINTR && errno != EAGAIN ) {
				die("Failed to read from stdin: %s.", strerror(errno));	
			} else {
				return false;
			}
		}
		count = (uint8_t)result;
	}
	if ( !count ) {
		if ( fd != STDIN_FILENO ) {
			end_upload();
		} else {
			quit = true;
		}
	}

	// On TTYs EOF is signaled with a ASCII end of transmission control character.
	if ( stdin_tty && fd == STDIN_FILENO ) {
		const uint8_t *eof = memchr(buffer, ASCII_EOT, count);
		if ( eof ) {
			count = (uint8_t)(eof - buffer);
			quit = true;
		}
	}

	// Optimize writes:
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
		die("Failed to advance TX write index.");
	}

	return true;
}

// The atexit(3) callback used by raw_mode_or_die()
static void
restore_stdin(void)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, orig);
}

// Put the standard input into raw mode if it's a TTY.
// Restore stdin to its previous mode on exit.
//
// TTYs have to be but into raw mode because in canonical
// mode lines are buffered and the forth system expects
// unbuffered input.
static void
raw_mode_or_die(void)
{
	if ( !stdin_tty ) {
		return;
	}

	if ( tcgetattr(STDIN_FILENO, orig) ) {
		die("Failed to get TTY attributes.");
	}
	atexit(restore_stdin);
	struct termios raw = *orig;
	raw.c_lflag &= (unsigned)~(ECHO | ICANON);
	if ( tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) ) {
		die("Failed to put terminal into raw mode: %s.", strerror(errno));
	}
}

// Calculate the difference between two timespecs.
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

// Put stdin into non blocking more or die trying.
//
// The protocol between STLINK/V2 and target and between
// the host PC and the STLINK/V2 both assume polling.
// Blocking on stdin would block transmissions from the
// target to the host PC as well.
static void
stdin_nonblock_or_die(void)
{
	errno = 0;
	int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	if ( errno ) {
		die("Failed to get file descriptor status flags: %s.", strerror(errno));
	}
	if ( fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == -1 ) {
		die("Failed to add O_NONBLOCK to file descriptor status flags: %s.", strerror(errno));
	}
}

// Retrieve the current clock value
static struct timespec
get_time(void)
{
	struct timespec ts;

	// No error handling required because reading valid clocks always succeeds.
	clock_gettime(CLOCK_MONOTONIC_FAST, &ts);

	return ts;
}

// Decode the ring buffer indicies to stderr.
//
// Calculates how much is free/used in each direction.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
static void __attribute__((unused))
debug_indicies(uint32_t indicies)
{
#pragma clang diagnostic pop
	uint8_t tx_w = (uint8_t)(indicies >>  0);
	uint8_t tx_r = (uint8_t)(indicies >>  8);
	uint8_t rx_w = (uint8_t)(indicies >> 16);
	uint8_t rx_r = (uint8_t)(indicies >> 24);
	uint8_t rx_u = rx_w - rx_r;
	uint8_t tx_f = 255 - (tx_w - tx_r);

	fprintf(stderr, "\nTX: r = %i, w = %i, f = %i RX: r = %i, w = %i, u = %i\n",
			tx_r, tx_w, tx_f, rx_r, rx_w, rx_u);
}


// Find out if stdin is a TTY or something else.
//
// Refuses files that aren't TTYs, pipes or regular files.
static void
stdin_file_type_or_die(void)
{
	struct stat sb;
	if ( fstat(STDIN_FILENO, &sb) ) {
		die("Failed to fstat() stdin: %s.", strerror(errno));
	}
	const mode_t file_type = sb.st_mode & S_IFMT;
	switch ( file_type ) {
		case S_IFIFO:
			stdin_pipe = true;
			break;

		case S_IFCHR:
			stdin_tty = isatty(STDIN_FILENO);
			if ( !stdin_tty ) {
				die("TTYs are the only supported kind of character device.");
			}
			break;
		case S_IFREG:
			stdin_file = true;
			break;
		default:
			die("unsupported file type: 0%o.", file_type);
	}
}

// Terminate the main loop by setting the quit flag.
static void
handler_term(int sig)
{
	(void)sig;
	quit = true;
}

// Schedule the target to be reset.
static void
handler_int(int sig)
{
	(void)sig;
	reset = true;
}

static void
handler_quit(int sig)
{
	(void)sig;
	upload = true;
}

// Register signal handlers for SIGINT and SIGTERM.
// The signal handlers terminate the main loop.
static void
install_signal_handlers(void)
{
	struct sigaction action[1] = {{ .sa_handler = handler_int }};
	sigemptyset(&action->sa_mask);
	sigaction(SIGINT, action, NULL);
	action->sa_handler = handler_term;
	sigaction(SIGTERM, action, NULL);
	action->sa_handler = handler_quit;
	sigaction(SIGQUIT, action, NULL);
}

int
main(int argc, char *argv[])
{
	// We need to know the base address of the ring buffer pair
	// on the target.
	//
	// Allow the user to skip automatic detection by providing the address.
	char * serial = NULL;
	switch ( argc ) {
		case 1:
			break;
		case 2:
			set_addr_or_die(argv[1]);
			break;
		case 3:
			set_addr_or_die(argv[1]);
			serial = argv[2];
			printf("\033]2;swd2 : %s\007", serial);
			fflush(stdout);
			break;
		default:
			fprintf(stderr, "usage: %s [<base-addr> [<serial>]]\n", argv[0]);
			return 64;
	}

	// Required setup code. Abort on all errors.
	stdin_file_type_or_die();
	install_signal_handlers();
	open_or_die(serial);
	raw_mode_or_die();
	stdin_nonblock_or_die();

	// Halt the target to read the base address from R11.
	if ( !addr ) {
		if ( stlink_force_debug(handle) ) {
			die("Failed to halt the target.");
		}
		struct stlink_reg regs[1];
		if ( stlink_read_reg(handle, 11, regs) ) {
			die("Failed to registers.");
		}
		if ( stlink_run(handle) ) {
			die("Failed to resume the target.");
		}
		addr = regs->r[11];
	}

	struct timespec last_active = get_time();
	while ( !quit ) {
		uint32_t indicies = read_indicies_or_die();
		bool rx_active = consume(indicies);
		bool tx_active = produce(indicies);
		bool active = rx_active | tx_active;
		struct timespec now = get_time();

		if ( reset ) {
			if ( stlink_reset(handle) ) {
				die("Failed to reset target.");
			}
			if ( stlink_run(handle) ) {
				die("Failed to resume target.");
			}
			fprintf(stderr, "\nRESET\n");
			reset = false;
		}

		if ( upload && fd == STDIN_FILENO ) {
			fd = open("upload.fs", O_RDONLY);
			if ( fd < 0 ) {
				fprintf(stderr, "*** Failed to open \"upload.fs\": %s. ***\n", strerror(errno));
				fd = STDIN_FILENO;
			}
			new_file = true;
			active = true;
			upload = false;
		}

		// Reduce polling rate after a period of inactivity saving CPU cycles and power.
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
