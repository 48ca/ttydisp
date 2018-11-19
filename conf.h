#define LOG_FILENAME "out.log"

/* Number of nanoseconds to spinlock (instead of sleeping) between frames.
 * Higher is more work for the CPU, lower is less accurate.
 */
#define SPINLOCK_NS 1E6

#define GREY_DIFF 15

// width / height
#define PIXEL_ASPECT_RATIO .5
