#include <poll.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>
#include <sodium.h>
#include <sys/queue.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <math.h>

#include "keycodes.h"


#define BUFSIZE 256                  // for device names and rescue key sequence
#define MAX_INPUTS 16                // number of devices to try autodetection
#define MAX_DEVICES 16               // max number of devices to read events from
#define MAX_RESCUE_KEYS 10           // max number of rescue keys to exit in case of emergency
#define MIN_KEYBOARD_KEYS 20         // need at least this many keys to be a keyboard
#define POLL_TIMEOUT_MS 1            // timeout to check for new events
#define DEFAULT_MAX_DELAY_MS 20      // upper bound on event delay
#define DEFAULT_MAX_NOISE 0          // max number of pixels of adversarial noise added to mouse movements
#define DEFAULT_STARTUP_DELAY_MS 500 // wait before grabbing the input device
#define _2PI 6.283185307
#define rel_mouse_move_with_obfuscation ev.type == EV_REL && max_noise != 0 && ev.value != 0 && (ev.code == REL_X || ev.code == REL_Y)
#define abs_mouse_move_with_obfuscation ev.type == EV_ABS && max_noise != 0 && ev.value != 0 && (ev.code == ABS_X || ev.code == ABS_Y) && x_axis_maxs[k] != 0 && y_axis_maxs[k] != 0

#define panic(format, ...) do { fprintf(stderr, format "\n", ## __VA_ARGS__); fflush(stderr); exit(EXIT_FAILURE); } while (0)

#ifndef min
#define min(a, b) ( ((a) < (b)) ? (a) : (b) )
#endif

#ifndef max
#define max(a, b) ( ((a) > (b)) ? (a) : (b) )
#endif


static int interrupt = 0;       // flag to interrupt the main loop and exit
static int verbose = 0;         // flag for verbose output


static char rescue_key_seps[] = ", ";  // delims to strtok
static char rescue_keys_str[BUFSIZE] = "KEY_LEFTSHIFT,KEY_RIGHTSHIFT,KEY_ESC";
static int rescue_keys[MAX_RESCUE_KEYS];  // Codes of the rescue key combo
static int rescue_len = 0;      // Number of rescue keys, set during initialization

static int max_delay = DEFAULT_MAX_DELAY_MS;  // lag will never exceed this upper bound
static int max_noise = DEFAULT_MAX_NOISE;
static int startup_timeout = DEFAULT_STARTUP_DELAY_MS;

static int device_count = 0;
static char named_inputs[MAX_INPUTS][BUFSIZE];

static int input_fds[MAX_INPUTS];
struct libevdev *output_devs[MAX_INPUTS];
int x_axis_maxs[MAX_INPUTS] = {0};
int y_axis_maxs[MAX_INPUTS] = {0};
int x_axis_mins[MAX_INPUTS] = {0};
int y_axis_mins[MAX_INPUTS] = {0};
struct libevdev_uinput *uidevs[MAX_INPUTS];

static struct option long_options[] = {
    {"read",    1, 0, 'r'},
    {"delay",   1, 0, 'd'},
    {"start",   1, 0, 's'},
    {"keys",    1, 0, 'k'},
    {"noise",    1, 0, 'n'},
    {"verbose", 0, 0, 'v'},
    {"help",    0, 0, 'h'},
    {0,         0, 0, 0}
};

TAILQ_HEAD(tailhead, entry) head;

struct entry {
    struct input_event iev;
    long time;
    TAILQ_ENTRY(entry) entries;
    int device_index;
    long int id;
};

void sleep_ms(long milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

long current_time_ms(void) {
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);
    return (spec.tv_sec) * 1000 + (spec.tv_nsec) / 1000000;
}

long random_between(long lower, long upper) {
    // default to max if the interval is not valid
    if (lower >= upper)
        return upper;

    return lower + randombytes_uniform(upper - lower + 1);
}



// from: https://github.com/luileito/mousefaker/blob/main/src/js/mousefaker.js
int noise(int position) {
        // generate a random number between 0 and 1 for u1 and u2
        double u1 = (float)randombytes_uniform(UINT32_MAX)/(float)(UINT32_MAX/1);
        double u2 = (float)randombytes_uniform(UINT32_MAX)/(float)(UINT32_MAX/1);
        
        double z0 = sqrt(-2.0 * log(u1)) * cos(_2PI * u2);
        
        int newPos = position + (z0 * max_noise);
        
        if (newPos < 0) {
                newPos = 0;
        }
        
        return newPos;
        
}


void set_rescue_keys(const char* rescue_keys_str) {
    char* _rescue_keys_str = malloc(strlen(rescue_keys_str) + 1);
    strncpy(_rescue_keys_str, rescue_keys_str, strlen(rescue_keys_str));
    _rescue_keys_str[strlen(rescue_keys_str)] = '\0';

    char* token = strtok(_rescue_keys_str, rescue_key_seps);

    while (token != NULL) {
        int keycode = lookup_keycode(token);
        if (keycode < 0) {
            panic("Invalid key name: '%s'\nSee keycodes.h for valid names", token);
        } else if (rescue_len < MAX_RESCUE_KEYS) {
            rescue_keys[rescue_len] = keycode;
            rescue_len++;
        } else {
            panic("Cannot set more than %d rescue keys", MAX_RESCUE_KEYS);
        }
        token = strtok(NULL, rescue_key_seps);
    }
    free(_rescue_keys_str);
}

int supports_event_type(int device_fd, int event_type) {
    unsigned long evbit = 0;
    // Get the bit field of available event types.
    ioctl(device_fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
    return evbit & (1 << event_type);
}

int supports_specific_key(int device_fd, unsigned int key) {
    size_t nchar = KEY_MAX/8 + 1;
    unsigned char bits[nchar];
    // Get the bit fields of available keys.
    ioctl(device_fd, EVIOCGBIT(EV_KEY, sizeof(bits)), &bits);
    return bits[key/8] & (1 << (key % 8));
}

int is_keyboard(int fd) {
    int key;
    int num_supported_keys = 0;

    // Only check devices that support EV_KEY events
    if (supports_event_type(fd, EV_KEY)) {
        // Count the number of KEY_* events that are supported
        for (key = 0; key <= KEY_MAX; key++) {
            if (supports_specific_key(fd, key)) {
                num_supported_keys += 1;
            }
        }
    }

    return (num_supported_keys > MIN_KEYBOARD_KEYS);
}

int is_mouse(int fd) {
    return (supports_event_type(fd, EV_REL) || supports_event_type(fd, EV_ABS));
}

void detect_devices() {
    int fd;
    char device[256];

    for (int i = 0; i < MAX_DEVICES; i++) {
        sprintf(device, "/dev/input/event%d", i);

        if ((fd = open(device, O_RDONLY)) < 0) {
            continue;
        }

        if (is_keyboard(fd)) {
            strncpy(named_inputs[device_count++], device, BUFSIZE-1);
            if (verbose)
                printf("Found keyboard at: %s\n", device);
        } else if (is_mouse(fd)) {
            strncpy(named_inputs[device_count++], device, BUFSIZE-1);
            if (verbose)
                printf("Found mouse at: %s\n", device);
        }

        close(fd);

        if (device_count >= MAX_INPUTS) {
            if (verbose)
                printf("Warning: ran out of inputs while detecting devices\n");
            break;
        }
    }
}

void init_inputs() {
    int fd;
    int one = 1;

    for (int i = 0; i < device_count; i++) {
        if ((fd = open(named_inputs[i], O_RDONLY)) < 0)
            panic("Could not open: %s", named_inputs[i]);

        // set the device to nonblocking mode
        if (ioctl(fd, FIONBIO, &one) < 0)
            panic("Could set to nonblocking: %s", named_inputs[i]);

        // grab the input device
        if (ioctl(fd, EVIOCGRAB, &one) < 0)
            panic("Could not grab: %s", named_inputs[i]);

        input_fds[i] = fd;
    }
}

void init_outputs() {
    for (int i = 0; i < device_count; i++) {
        int err = libevdev_new_from_fd(input_fds[i], &output_devs[i]);

        if (err != 0)
            panic("Could not create evdev for input device: %s", named_inputs[i]);
        
        if(supports_event_type(input_fds[i], EV_ABS)) {
            // get_abs_max will return 0 on failure, instead of exiting, main_loop will just not add mouse movement obfuscation if device reports that axis is not valid
            int abs_x_max = libevdev_get_abs_maximum(output_devs[i], ABS_X);
            x_axis_maxs[i] = abs_x_max;
            
            int abs_y_max = libevdev_get_abs_maximum(output_devs[i], ABS_Y);
            y_axis_maxs[i] = abs_y_max;
            
            int abs_x_min = libevdev_get_abs_minimum(output_devs[i], ABS_X);
            x_axis_mins[i] = abs_x_min;
            
            int abs_y_min = libevdev_get_abs_minimum(output_devs[i], ABS_Y);
            y_axis_mins[i] = abs_y_min;
            
        }

        err = libevdev_uinput_create_from_device(output_devs[i], LIBEVDEV_UINPUT_OPEN_MANAGED, &uidevs[i]);

        if (err != 0)
            panic("Could not create uidev for input device: %s", named_inputs[i]);
    }
}

void emit_event(struct entry *e) {
    int res, delay;
    long now = current_time_ms();
    delay = (int) (e->time - now);

    res = libevdev_uinput_write_event(uidevs[e->device_index], e->iev.type, e->iev.code, e->iev.value);
    if (res != 0) {
        panic("Failed to write event to uinput: %s", strerror(-res));
    }


    if (verbose) {
        printf("Released event at time : %ld. Device: %d,  Type: %*d,  "
               "Code: %*d,  Value: %*d,  Missed target:  %*d ms \n",
               e->time, e->device_index, 3, e->iev.type, 5, e->iev.code, 5, e->iev.value, 5, delay);
    }
}

void main_loop() {
    int err;
    long int id = 0;
    int can_obfuscate = 0;
    long prev_release_time = 0;
    long current_time = 0;
    long lower_bound = 0;
    long random_delay = 0;
    uint32_t abs_last_x = 0;
    uint32_t abs_last_y = 0;
    struct input_event ev;
    struct entry *n1, *np;
    
    // only used on mouse moves
    struct input_event ev2, ev3, ev4, ev5, ev6;
    struct entry *n2 = NULL, *n3 = NULL, *n4 = NULL, *n5 = NULL, *n6 = NULL;


    // initialize the rescue state
    int rescue_state[MAX_RESCUE_KEYS];
    for (int i = 0; i < rescue_len; i++) {
        rescue_state[i] = 0;
    }

    // load input file descriptors for polling
    struct pollfd *pfds = calloc(device_count, sizeof(struct pollfd));
    if (pfds == NULL) {
        panic("Failed to allocate memory for pollfd array");
    }
    for (int j = 0; j < device_count; j++) {
        pfds[j].fd = input_fds[j];
        pfds[j].events = POLLIN;
    }

    // the main loop breaks when the rescue keys are detected
    // On each iteration, wait for input from the input devices
    // If the event is a key press/release, then schedule for
    // release in the future by generating a random delay. The
    // range of the delay depends on the previous event generated
    // so that events are always scheduled in the order they
    // arrive (FIFO).
    while (!interrupt) {
        can_obfuscate = 0;
        // Emit any events exceeding the current time
        current_time = current_time_ms();
        while ((np = TAILQ_FIRST(&head)) && (current_time >= np->time)) {
            emit_event(np);
            TAILQ_REMOVE(&head, np, entries);
            free(np);
        }

        // Wait for next input event
        if ((err = poll(pfds, device_count, POLL_TIMEOUT_MS)) < 0)
            panic("poll() failed: %s\n", strerror(errno));

        // timed out, do nothing
        if (err == 0)
            continue;

        // An event is available, mark the current time
        current_time = current_time_ms();

        // Buffer the event with a random delay
        for (int k = 0; k < device_count; k++) {
            if (pfds[k].revents & POLLIN) {
                if ((err = read(pfds[k].fd, &ev, sizeof(ev))) <= 0)
                    panic("read() failed: %s", strerror(errno));

                // check for the rescue sequence.
                if (ev.type == EV_KEY) {
                    int all = 1;
                    for (int j = 0; j < rescue_len; j++) {
                        if (rescue_keys[j] == ev.code)
                            rescue_state[j] = (ev.value == 0 ? 0 : 1);
                        all = all && rescue_state[j];
                    }
                    if (all)
                        interrupt = 1;
                }

                // schedule the keyboard event to be released sometime in the future.
                // lower bound must be bounded between time since last scheduled event and max delay
                // preserves event order and bounds the maximum delay
                lower_bound = min(max(prev_release_time - current_time, 0), max_delay);
                
                
                // syn events are not delayed
                if (ev.type == EV_SYN) {
                    random_delay = lower_bound;
                } else {
                    random_delay = random_between(lower_bound, max_delay);
                }
                
                if(rel_mouse_move_with_obfuscation) {
                    if(ev.code == REL_X) {

                        // select a random midpoint to add the perpendicular move
                        int mid_point = random_between(1, abs(ev.value));

                        int final_move = abs(ev.value) - mid_point;

                        if(ev.value < 0) {
                            mid_point *= -1;
                            final_move *= -1;
                        }

                        int pixels_y = random_between(1, max_noise);

                        // randomly decide whether y move will be up or down
                        if(random_between(0, 1)) {
                            pixels_y *= -1;
                        }


                        ev.type = EV_REL;
                        ev.code = REL_X;
                        ev.value = mid_point;

                        ev2.type = EV_REL;
                        ev2.code = REL_Y;
                        ev2.value = pixels_y;

                        ev3.type = EV_SYN;
                        ev3.code = 0;
                        ev3.value = 0;

                        ev4.type = EV_REL;
                        ev4.code = REL_Y;
                        ev4.value = pixels_y * -1;

                        ev5.type = EV_REL;
                        ev5.code = REL_X;
                        ev5.value = final_move;
                                
                        ev6.type = EV_SYN;
                        ev6.code = 0;
                        ev6.value = 0;
                                        
                    } else if(ev.code == REL_Y) {
                        // select a random midpoint to add the perpendicular move
                        int mid_point = random_between(1, abs(ev.value));

                        int final_move = abs(ev.value) - mid_point;

                        if(ev.value < 0) {
                            mid_point *= -1;
                            final_move *= -1;
                        }

                        int pixels_x = random_between(1, max_noise);

                        // randomly decide whether x move will be left or right
                        if(random_between(0, 1)) {
                            pixels_x *= -1;
                        }



                        ev.type = EV_REL;
                        ev.code = REL_Y;
                        ev.value = mid_point;

                        ev2.type = EV_REL;
                        ev2.code = REL_X;
                        ev2.value = pixels_x;

                        ev3.type = EV_SYN;
                        ev3.code = 0;
                        ev3.value = 0;

                        ev4.type = EV_REL;
                        ev4.code = REL_X;
                        ev4.value = pixels_x * -1;

                        ev5.type = EV_REL;
                        ev5.code = REL_Y;
                        ev5.value = final_move;
                                
                        ev6.type = EV_SYN;
                        ev6.code = 0;
                        ev6.value = 0;
                    }
                }
                
                
                if(abs_mouse_move_with_obfuscation) {
                        
                    // either last known x or y position is not known yet, needed before obfuscation can be added
                    if(abs_last_x == 0 || abs_last_y == 0) {
                        if(ev.code == ABS_X) {
                            abs_last_x = ev.value;
                        } else if(ev.code == ABS_Y) {
                            abs_last_y = ev.value;
                        }
                                
                        can_obfuscate = 0;
                                
                                
                    } else {
                        if(ev.code == ABS_X) {
                            uint32_t origPos = ev.value; 
                                        
                            // modified ABS_X
                            ev.type = EV_ABS;
                            ev.code = ABS_X;
                            int newPos = noise(ev.value);
                            if(newPos > x_axis_maxs[k]) {
                                newPos = x_axis_maxs[k];
                            } else if(newPos < x_axis_mins[k]) {
                                newPos = x_axis_mins[k];
                            }
                            ev.value = newPos;
                                        
                            // modified ABS_Y based on last known position
                            ev2.type = EV_ABS;
                            ev2.code = ABS_Y;
                            newPos = noise(abs_last_y);
                            if(newPos > y_axis_maxs[k]) {
                                newPos = y_axis_maxs[k];
                            } else if(newPos < y_axis_mins[k]) {
                                newPos = y_axis_mins[k];
                            }
                            ev2.value = newPos;
                        
                        
                            ev3.type = EV_SYN;
                            ev3.code = 0;
                            ev3.value = 0;
                        
                            // move to the target x position
                            ev4.type = EV_ABS;
                            ev4.code = ABS_X;
                            ev4.value = origPos;
                        
                            // move back to the original y position
                            ev5.type = EV_ABS;
                            ev5.code = ABS_Y;
                            ev5.value = abs_last_y;
                        
                            ev6.type = EV_SYN;
                            ev6.code = 0;
                            ev6.value = 0;
                        
                            abs_last_x = origPos;
                                                

                                        
                        } else if(ev.code == ABS_Y) {
                            uint32_t origPos = ev.value; 
                                        
                            // modified ABS_Y
                            ev.type = EV_ABS;
                            ev.code = ABS_Y;
                            int newPos = noise(ev.value);
                            if(newPos > y_axis_maxs[k]) {
                                newPos = y_axis_maxs[k];
                            } else if(newPos < y_axis_mins[k]) {
                                newPos = y_axis_mins[k];
                            }
                            ev.value = newPos;
                        
                            // modified ABS_X based on last known position
                            ev2.type = EV_ABS;
                            ev2.code = ABS_X;
                            newPos = noise(abs_last_x);
                            if(newPos > x_axis_maxs[k]) {
                                newPos = x_axis_maxs[k];
                            } else if(newPos < x_axis_mins[k]) {
                                newPos = x_axis_mins[k];
                            }
                            ev2.value = newPos;
                        
                            ev3.type = EV_SYN;
                            ev3.code = 0;
                            ev3.value = 0;
                        
                            // move to the target x position
                            ev4.type = EV_ABS;
                            ev4.code = ABS_Y;
                            ev4.value = origPos;
                        
                            // move back to the original y position
                            ev5.type = EV_ABS;
                            ev5.code = ABS_X;
                            ev5.value = abs_last_x;
                        
                            ev6.type = EV_SYN;
                            ev6.code = 0;
                            ev6.value = 0;
                        
                            abs_last_y = origPos;
                        }
                                
                                
                        can_obfuscate = 1;
                    }
                        
                        
                }


                // Buffer the event
                n1 = malloc(sizeof(struct entry));
                if (n1 == NULL) {
                        panic("Failed to allocate memory for entry");
                }
                n1->time = current_time + random_delay;
                n1->iev = ev;
                n1->device_index = k;
                if(id < LONG_MAX-1) {
                        n1->id = id++;
                } else {
                        id = 0;
                        n1->id = id++;
                }
                            
                TAILQ_INSERT_TAIL(&head, n1, entries);

                // Keep track of the previous scheduled release time
                prev_release_time = n1->time;
                
                            
                if((rel_mouse_move_with_obfuscation) || (abs_mouse_move_with_obfuscation && can_obfuscate) ) {
                        long random_delay = random_between(lower_bound, max_delay);
                        n2 = malloc(sizeof(struct entry));
                        n3 = malloc(sizeof(struct entry));
                        n4 = malloc(sizeof(struct entry));
                        n5 = malloc(sizeof(struct entry));
                        n6 = malloc(sizeof(struct entry));
                        
                        if(n2 == NULL || n3 == NULL || n4 == NULL || n5 == NULL || n6 == NULL) {
                            panic("Failed to allocate memory for either n2, n3, n4, n5, or n6");
                        }
                        

                        n2->time = n1->time + (long) random_delay;
                        n2->iev = ev2;
                        n2->device_index = k;
                        if(id < LONG_MAX-1) {
                                n2->id = id++;
                        } else {
                                id = 0;
                                n2->id = id++;
                        }
                        TAILQ_INSERT_TAIL(&head, n2, entries);
                        

                        

                        random_delay = random_between(lower_bound, max_delay);
                        
                        n3->time = n2->time + (long) random_delay;
                        n3->iev = ev3;
                        n3->device_index = k;
                        if(id < LONG_MAX-1) {
                                n3->id = id++;
                        } else {
                                id = 0;
                                n3->id = id++;
                        }
                        TAILQ_INSERT_TAIL(&head, n3, entries);

                        random_delay = random_between(lower_bound, max_delay);
                        
                        n4->time = n3->time + (long) random_delay;
                        n4->iev = ev4;
                        n4->device_index = k;
                        if(id < LONG_MAX-1) {
                                n4->id = id++;
                        } else {
                                id = 0;
                                n4->id = id++;
                        }
                        TAILQ_INSERT_TAIL(&head, n4, entries);

                        random_delay = random_between(lower_bound, max_delay);
                        
                        n5->time = n4->time + (long) random_delay;
                        n5->iev = ev5;
                        n5->device_index = k;
                        if(id < LONG_MAX-1) {
                                n5->id = id++;
                        } else {
                                id = 0;
                                n5->id = id++;
                        }
                        TAILQ_INSERT_TAIL(&head, n5, entries);
                        
                        random_delay = random_between(lower_bound, max_delay);
                        
                        n6->time = n5->time + (long) random_delay;
                        n6->iev = ev6;
                        n6->device_index = k;
                        if(id < LONG_MAX-1) {
                                n6->id = id++;
                        } else {
                                id = 0;
                                n6->id = id++;
                        }
                        TAILQ_INSERT_TAIL(&head, n6, entries);
                }
                
                // on mouse moves
                if((rel_mouse_move_with_obfuscation) || (abs_mouse_move_with_obfuscation  && can_obfuscate) ) {
                        prev_release_time = n6->time;
                }

                if (verbose) {
                        printf("Buffered n1 event at time: %ld. Device: %d,  Type: %*d,  "
                                "Code: %*d,  Value: %*d, id: %ld,  Scheduled delay: %*ld ms \n",
                                n1->time, k, 3, n1->iev.type, 5, n1->iev.code, 5, n1->iev.value, n1->id,
                                4, random_delay);
                        if (lower_bound > 0) {
                                printf("Lower bound raised to: %*ld ms\n", 4, lower_bound);
                        }
                        if(((rel_mouse_move_with_obfuscation) || (abs_mouse_move_with_obfuscation  && can_obfuscate)) && n2 && n3 && n4 && n5 && n6 ) {
                                printf("Buffered n2 event at time: %ld. Device: %d,  Type: %*d,  "
                                "Code: %*d,  Value: %*d, id: %ld,  Scheduled delay: %*ld ms \n",
                                n2->time, k, 3, n2->iev.type, 5, n2->iev.code, 5, n2->iev.value, n2->id,
                                4, random_delay);
                                printf("n2 type %d\n", n2->iev.type);
                                printf("Buffered n3 event at time: %ld. Device: %d,  Type: %*d,  "
                                "Code: %*d,  Value: %*d, id: %ld,  Scheduled delay: %*ld ms \n",
                                n3->time, k, 3, n3->iev.type, 5, n3->iev.code, 5, n3->iev.value, n3->id,
                                4, random_delay);
                                printf("Buffered n4 event at time: %ld. Device: %d,  Type: %*d,  "
                                "Code: %*d,  Value: %*d, id: %ld,  Scheduled delay: %*ld ms \n",
                                n4->time, k, 3, n4->iev.type, 5, n4->iev.code, 5, n4->iev.value, n4->id,
                                4, random_delay);
                                printf("Buffered n5 event at time: %ld. Device: %d,  Type: %*d,  "
                                "Code: %*d,  Value: %*d, id: %ld,  Scheduled delay: %*ld ms \n",
                                n5->time, k, 3, n5->iev.type, 5, n5->iev.code, 5, n5->iev.value, n5->id,
                                4, random_delay);
                                printf("Buffered n6 event at time: %ld. Device: %d,  Type: %*d,  "
                                "Code: %*d,  Value: %*d, id: %ld,  Scheduled delay: %*ld ms \n",
                                n6->time, k, 3, n6->iev.type, 5, n6->iev.code, 5, n6->iev.value, n6->id,
                                4, random_delay);
                                
                        }
                }
            }


        }
    }
    
    free(pfds);
}

    


void usage() {
    fprintf(stderr, "Usage: kloak [options]\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -r filename: device file to read events from. Can specify multiple -r options.\n");
    fprintf(stderr, "  -d delay: maximum delay (milliseconds) of released events. Default 100.\n");
    fprintf(stderr, "  -s startup_timeout: time to wait (milliseconds) before startup. Default 100.\n");
    fprintf(stderr, "  -k csv_string: csv list of rescue key names to exit kloak in case the\n"
            "     keyboard becomes unresponsive. Default is 'KEY_LEFTSHIFT,KEY_RIGHTSHIFT,KEY_ESC'.\n");
    fprintf(stderr, "  -v: verbose mode\n");
    fprintf(stderr, "  -n: max noise added to mouse movements in pixels. Default %d, can fully disable by setting to 0\n", max_noise);
}

void banner() {
    printf("********************************************************************************\n"
           "* Started kloak : Keystroke-level Online Anonymizing Kernel\n"
           "* Maximum delay : %d ms\n"
           "* Reading from  : %s\n",
           max_delay, named_inputs[0]);

    for (int i = 1; i < device_count; i++) {
        printf("*                 %s\n", named_inputs[i]);
    }

    printf("* Rescue keys   : %s", lookup_keyname(rescue_keys[0]));
    for (int i = 1; i < rescue_len; i++) {
        printf(" + %s", lookup_keyname(rescue_keys[i]));
    }

    printf("\n");
    printf("********************************************************************************\n");
}

int main(int argc, char **argv) {
    if (sodium_init() == -1) {
        panic("sodium_init failed");
    }

    if ((getuid()) != 0)
        printf("You are not root! This may not work...\n");

    while (1) {
        int c = getopt_long(argc, argv, "r:d:s:k:n:vh", long_options, NULL);

        if (c < 0)
            break;

        switch (c) {
        case 'r':
            if (device_count >= MAX_INPUTS)
                panic("Too many -r options: can read from at most %d devices\n", MAX_INPUTS);
            strncpy(named_inputs[device_count++], optarg, BUFSIZE-1);
            break;

        case 'd':
            if ((max_delay = atoi(optarg)) < 0)
                panic("Maximum delay must be >= 0\n");
            break;

        case 's':
            if ((startup_timeout = atoi(optarg)) < 0)
                panic("Startup timeout must be >= 0\n");
            break;

        case 'k':
            strncpy(rescue_keys_str, optarg, BUFSIZE-1);
            break;

        case 'v':
            verbose = 1;
            break;
            
        case 'n':
            if((max_noise = atoi(optarg)) < 0) {
                panic("Maximum noise must be >= 0");
            }
            break;

        case 'h':
            usage();
            exit(0);
            break;

        default:
            usage();
            panic("Unknown option: %c \n", optopt);
        }
    }

    // autodetect devices if none were specified
    if (device_count == 0)
        detect_devices();


    // autodetect failed
    if (device_count == 0)
        panic("Unable to find any keyboards or mice. Specify which input device(s) to use with -r");


    // set rescue keys from the default sequence or -k arg
    set_rescue_keys(rescue_keys_str);

    // wait for pending events to finish, avoids keys being "held down"
    printf("Waiting %d ms...\n", startup_timeout);
    sleep_ms(startup_timeout);

    // open the input devices and create the output devices
    init_inputs();
    init_outputs();

    // initialize the event queue
    TAILQ_INIT(&head);

    banner();
    main_loop();

    // close everything
    for (int i = 0; i < device_count; i++) {
        libevdev_uinput_destroy(uidevs[i]);
        libevdev_free(output_devs[i]);
        close(input_fds[i]);
    }

 
    exit(EXIT_SUCCESS);

}
