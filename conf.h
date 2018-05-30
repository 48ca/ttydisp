#define LOG_FILENAME "out.log"

/* Number of nanoseconds to spinlock (instead of sleeping) between frames.
 * Higher is more work for the CPU, lower is less accurate.
 */
#define SPINLOCK_NS 1E6
