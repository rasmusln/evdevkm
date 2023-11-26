#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <argp.h>
#include <libgen.h>
#include <signal.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#define KEY_CODE_ARRAY_LENGTH 243
#define MAX_EVENTS 10

char *label_host = "host";
char *label_guest = "guest";

const char *argp_program_version = "0.0.1";
const char *argp_program_bug_address = "/dev/null";

static char doc[] = "Virtual keyboard and mouse switch.\n\n"
	"The switch capabilities extend to any device under '/dev/input'"
	" and the switch key can be specified as an option."
	" The intended use is with the '-g' option as this will grab original devices"
	" and then route the input events to either the 'host' virtual devices or the"
	" 'guest' virtual devices";

static char args_doc[] = "[Device...]";

static struct argp_option options[] = {
	{ "verbose", 'v', 0, 0, "Verbose output" },
	{ "print-key-codes", 'p', 0, 0, "Print key codes" },
	{ "grab", 'g', 0, 0, "Grab device" },
	{ "no-symlink", 'n', 0, 0, "Create no symlinks" },
	{ "user", 'u', "UID_OR_USER", 0, "Uid or user name to assign to guest device" },
	{ "code", 'c', "KEY_OR_CODE", 0, "Key name or key code to be used as switch" },
	{ 0 }
};

struct KeyCode {
	char *key;
	unsigned int code;
};

void key_code_print_key_codes();
int key_code_parse(unsigned int *code, char *name_or_code);
const struct KeyCode* key_code_by_code(unsigned int code);
const struct KeyCode key_codes[];

enum TARGET {
	initialized,
	host,
	guest
};

char* target_label(enum TARGET target) {
	switch (target) {
		case initialized:
		case host:
			return label_host;
		case guest:
			return label_guest;
	}
}

enum TARGET flip_target(enum TARGET target) {
	switch (target) {
		case host:
			return guest;
		case guest:
		case initialized:
			return host;
	}
}

struct DeviceTarget {
	struct libevdev_uinput *uidev;
	char *symlink_path;
};

struct Options {
	bool verbose;
	bool grab;
	bool no_symlink;
	bool is_uid_set;
	unsigned int key_code;
	uid_t uid;
};

struct Device {
	char *device_path;

	int device_fd;
	struct libevdev *device;

	struct DeviceTarget host;
	struct DeviceTarget guest;

	struct Device *next;

	struct Options options;
};

struct arguments {
	struct Device *head;
	struct Options options;
};


struct DeviceTarget* device_target(struct Device *d, enum TARGET target) {
	switch (target) {
		case host:
			return &d->host;
		case guest:
			return &d->guest;
	}
}

int is_valid(struct Device *device) {
	struct stat st;

	if (stat(device->device_path, &st) < 0 && S_ISREG(st.st_mode) == false) {
		return false;
	}

	return true;
}

bool is_only_digit(char *s) {
	for (int i = 0; i < strlen(s); i++) {
		if (!isdigit(s[i])) {
			return false;
		}
	}
	return true;
}

int uid_from_string(uid_t *uid, char *uid_or_user_name) {
	struct passwd *pwd;
	if (is_only_digit(uid_or_user_name)) {
		uid_t i = strtol(uid_or_user_name, NULL, 10);
		pwd = getpwuid(i);
	} else {
		pwd = getpwnam(uid_or_user_name);
	}

	if (pwd == NULL) {
		fprintf(stderr, "failed to find user for '%s'\n", uid_or_user_name);
		return -1;
	}

	*uid = pwd->pw_uid;

	return 0;
}

void free_device_target(struct DeviceTarget *t) {
	if (t->uidev != NULL) {
		libevdev_uinput_destroy(t->uidev);
		t->uidev = NULL;
	}

	if (t->symlink_path) {
		free(t->symlink_path);
		t->symlink_path = NULL;
	}
}

void free_device(struct Device *device) {
	if (device->device_path != NULL) {
		free(device->device_path);
		device->device_path = NULL;
	}

	if (device->device_fd != -1) {
		close(device->device_fd);
		device->device_fd = -1;
	}

	free_device_target(&device->host);
	free_device_target(&device->guest);

	free(device);
}

int epoll_add(int epfd, int fd, void *ptr) {
	struct epoll_event ev;

	ev.events = EPOLLIN;

	// note that data is an union and defaults to fd if ptr == NULL
	if (ptr != NULL) {
	  ev.data.ptr = ptr;
	} else {
		ev.data.fd = fd;
	}

	return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int initialize_symlink_path(struct Device *d, enum TARGET target) {
	int rc = 0;
	char *path, *name, *label;
	size_t size;
	struct DeviceTarget *t;

	path = "/dev/input/by-path";
	name = basename(d->device_path);

	t = device_target(d, target);

	label = target_label(target);

	size = strlen(path)+strlen(name)+strlen(label)+4;

	t->symlink_path = malloc(sizeof(char)*size);
	if (t->symlink_path == NULL) {
		return -1;
	}

	rc = snprintf(t->symlink_path, size, "%s/%s-%s", path, name, label);
	if (rc < 0) {
		free(t->symlink_path);
		return rc;
	}

	return 0;
}

int file_exists(char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

int initialize_symlink(struct Device *d, struct Options *options,  enum TARGET target) {
	int rc;

	struct DeviceTarget *t = device_target(d, target);

	rc = initialize_symlink_path(d, target);
	if (rc < 0) { 
		fprintf(stderr, "failed to generate symlink path\n");
	}

	if (file_exists(t->symlink_path)); {
		rc = remove(t->symlink_path);
		if (rc < 0) {
			fprintf(stderr, "failed to remove %s with code %d\n", t->symlink_path, rc);
		}
	}

	rc = symlink(libevdev_uinput_get_devnode(t->uidev), t->symlink_path);
	if (rc < 0) {
		fprintf(stderr, "symlink creation failed for %s -> %s\n", 
			t->symlink_path, 
			libevdev_uinput_get_devnode(t->uidev));
		return rc;
	}

	if (options->is_uid_set && target == guest) {
		rc = chown(libevdev_uinput_get_devnode(t->uidev), options->uid, -1);
		if (rc < 0) {
			fprintf(stderr, "failed to set uid for %s\n", libevdev_uinput_get_devnode(t->uidev));
			return rc;
		}
	}

	return 0;
}

int initialize_target(struct Device *device, struct Options *options, enum TARGET target) {
	int rc;
	char *label;
	struct DeviceTarget *t = device_target(device, target);

	label = target_label(target);

	rc = libevdev_uinput_create_from_device(device->device, LIBEVDEV_UINPUT_OPEN_MANAGED, &(t->uidev));
	if (rc < 0) {
		fprintf(stderr, "failed to create %s input\n", label);
		return rc;
	}

	if (options->verbose) {
		fprintf(stderr, "create uinput device: %s\n", libevdev_uinput_get_devnode(t->uidev));
	}

	if (!options->no_symlink) {
		rc = initialize_symlink(device, options, target);
		if (rc < 0) {
			return 0;
		}
	}

	return 0;
}

static bool switch_at_next_ev_syn = false;
static int number_of_keys_pressed = 0;

/**
 * Switch and relay events to the target device.
 *
 * The target device is switched by `options->key_code` after the next `EV_SYN` event
 * when no keys are pressed.
 */
int switch_and_relay_event(struct Device *device, struct Options *options, enum TARGET *target, struct input_event *ev) {
	int rc;
	enum TARGET previous_target;
	char *from, *to;

	if (ev->type == EV_KEY && ev->value == 1) {
		number_of_keys_pressed++;
	} else if (ev->type == EV_KEY && ev->value == 0 && number_of_keys_pressed > 0) {
		number_of_keys_pressed--;
	}

	if (options->verbose) {
		printf("event: %s %s %d\n",
			libevdev_event_type_get_name(ev->type),
			libevdev_event_code_get_name(ev->type, ev->code),
			ev->value);

		printf("#keys: %d\n", number_of_keys_pressed);
	}

	if (ev->type == EV_KEY && ev->code == options->key_code && ev->value == 1) {
		switch_at_next_ev_syn = true;
	}

	switch (*target) {
		case guest:
			rc = libevdev_uinput_write_event(device->guest.uidev, ev->type, ev->code, ev->value);
			if (rc < 0) {
				fprintf(stderr, "failed write event\n");
				return rc;
			}
			break;
		case host:
		default:
			rc = libevdev_uinput_write_event(device->host.uidev, ev->type, ev->code, ev->value);
			if (rc < 0) {
				fprintf(stderr, "failed write event\n");
				return rc;
			}
			break;
	}

	if (ev->type == EV_SYN && grab_at_next_ev_syn && number_of_keys_pressed == 0) {
		// if initialized then grab device
		if (*target == initialized && options->grab) {
			rc = libevdev_grab(device->device, LIBEVDEV_GRAB);
			if (rc < 0) {
				fprintf(stderr, "failed to grab device");
				return rc;
			} 

			if (options->verbose) {
				printf("grabbed device %s\n", device->device_path);
			}
		}

		previous_target = *target;
		*target = flip_target(*target);

		if (options->verbose) {
			from = target_label(previous_target);
			to = target_label(*target);

			printf("flipped target from %s to %s\n", from, to);
		}
		
		switch_at_next_ev_syn = false;
	}

	return 0;
}

int next_events(struct Device *device, struct Options *options, enum TARGET *target, unsigned int flag) {
	int rc;
	struct input_event ev;
	unsigned int f = flag;

	while (true) {
		rc = libevdev_next_event(device->device, f, &ev);
		switch (rc) {
			case LIBEVDEV_READ_STATUS_SUCCESS:
				rc = switch_and_relay_event(device, options, target, &ev);
				if (rc < 0) {
					return rc;
				}			

				if (options->verbose) {
					printf("next event -> status success\n");
				}
				break;
			case LIBEVDEV_READ_STATUS_SYNC:
				if (f != LIBEVDEV_READ_FLAG_FORCE_SYNC) {
					rc = switch_and_relay_event(device, options, target, &ev);
					if (rc < 0) {
						return rc;
					}
				}

				f = LIBEVDEV_READ_FLAG_SYNC;

				if (options->verbose) {
					printf("next event -> status sync\n");
				}
				break;
			case -EAGAIN:
				return 0;
			default: 
				return rc;
		}

	
	}
}

int initialize(struct Device *device, struct Options *options,  int epfd) {
	int rc, uifd;

	device->device_fd = open(device->device_path, O_RDONLY|O_NONBLOCK);
	if (device->device_fd < 0) {
		fprintf(stderr, "failed to on %s\n", device->device_path);
		return -1;
	}

	rc = libevdev_new_from_fd(device->device_fd, &(device->device));
	if (rc < 0) {
		fprintf(stderr, "failed to initialize %s (%d)\n", device->device_path, rc);
		return rc;
	}


	rc = initialize_target(device, options, host);
	if (rc < 0) {
		return 0;
	}

	rc = initialize_target(device, options, guest);
	if (rc < 0) {
		return 0;
	}

	rc = epoll_add(epfd, device->device_fd, device); 
	if (rc < 0) {
		fprintf(stderr, "failed to poll %s\n", device->device_path);
		return rc;
	}

	return 0;
}

int create(struct Device **device, char *device_path) {
	int rc;
	struct Device *d;

	d = malloc(sizeof(struct Device));
	if (d == NULL) {
		return -1;
	}

	d->device_path = malloc(strlen(device_path)+1);
	if (d->device_path == NULL) {
		return -1;
	}

	strcpy(d->device_path, device_path);

	d->device_fd = -1;
	d->device = NULL;

	d->host.uidev = NULL;
	d->host.symlink_path = NULL;

	d->guest.uidev = NULL;
	d->guest.symlink_path = NULL;

	d->next = NULL;

	(*device) = d;

	return 0;
}

void append(struct Device **head, struct Device *device) {
	struct Device *d, *p;

	p = NULL;
	d = *head;

	while (d != NULL) {
		p = d;
		d = d->next;
	}

	d = device;

	if (p != NULL) {
		p->next = d;
	}

	if (*head == NULL) {
		*head = d;
	}
}


void free_all_devices(struct Device *head) {
	struct Device *d;

	if (head != NULL) {
		do {
			d = head;
			head = head->next;

			free(d);
			d = NULL;
		} while (head != NULL);
	}
}

int block_signals(int epfd) {
	int rc, signal_fd;
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd == -1) {
		fprintf(stderr, "failed to create signal file descriptor\n");
		return -1;
	}

	rc = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to create sigprocmask\n");
		close(signal_fd);
		return rc;
	}

	rc = epoll_add(epfd, signal_fd, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to add signal file descriptor to epoll\n");
		close(signal_fd);
		return rc;
	}

	return signal_fd;
}

void cleanup(struct Device *head, int *epfd, int *signal_fd) {
	free_all_devices(head);

	if (!(*epfd < 0)) {
		close(*epfd);
		*epfd = -1;
	}

	if (!(*signal_fd < 0)) {
		close(*signal_fd);
		*signal_fd = -1;
	}
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	int rc = 0;

	struct arguments *arguments = state->input;

	switch (key) {
		case 'v':
			arguments->options.verbose = true;
			break;
		case 'g':
			arguments->options.grab = true;
			break;
		case 'n':
			arguments->options.no_symlink = true;
			break;
		case 'u':
			rc = uid_from_string(&(arguments->options.uid), arg);
			if (rc < 0) {
				fprintf(stderr, "%s is not a valid uid or user name\n", arg);
				return ARGP_HELP_STD_USAGE;
			}
			arguments->options.is_uid_set = true;
			break;
		case 'p':
			key_code_print_key_codes();
			rc = -1;
			break;
		case 'c':
			unsigned int code;
			rc = key_code_parse(&code, arg);
			if (rc < 0) {
				fprintf(stderr, "%s is not a key name or key code\n", arg);
				break;
			}
			arguments->options.key_code = code;
			break;
		case ARGP_KEY_ARG:
			struct Device *d;

			rc = create(&d, arg);
			if (rc < 0) {
				rc = ARGP_HELP_STD_USAGE;
				break;
			}

			if (!is_valid(d)) {
				fprintf(stderr, "%s is not a valid device\n", arg);
				rc = ARGP_HELP_STD_ERR;
				break;
			}

			append(&(arguments->head), d);
			break;
	}

	if (rc < 0) {
		free_all_devices(arguments->head);
	}

	return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc };

int main(int argc, char **argv) {
	int rc, epfd = -1, signal_fd = -1, nfds, n;
	struct arguments arguments;
	struct Device *head, *d;
	struct Options options;
	struct epoll_event events[MAX_EVENTS];
	enum TARGET target = initialized;

	arguments.head = NULL;
	arguments.options.verbose = false;
	arguments.options.grab = false;
	arguments.options.no_symlink = false;
	arguments.options.is_uid_set = false;
	arguments.options.key_code = KEY_RIGHTSHIFT;

	argp_parse(&argp, argc, argv, 0, 0, &arguments);

	head = arguments.head;
	options = arguments.options;

	if (options.verbose) {
		const struct KeyCode *key_code = key_code_by_code(options.key_code);
		printf("Switch key: %s\n", key_code->key);
	}

	if (head != NULL) {

		epfd = epoll_create1(0);
		if (epfd < 0) {
			fprintf(stderr, "failed to create epoll file descriptor\n");
			cleanup(arguments.head,  &epfd, &signal_fd);
		};

		for (d = head; d != NULL; d = d->next) {
			if (!is_valid(d)) { 
				fprintf(stderr, "device %s is invalid\n", d->device_path);
				cleanup(head, &epfd, &signal_fd);
				exit(1);
			}

			if (initialize(d, &options, epfd) < 0) {
				fprintf(stderr, "device %s failed to initialize\n", d->device_path);
				cleanup(head,  &epfd, &signal_fd);
				exit(1);
			}

			if (force_sync(d, &options, &target) < 0) {
				fprintf(stderr, "device %s failed to force sync\n", d->device_path);
			}
		}

		signal_fd = block_signals(epfd);
		if (signal_fd < 0) {
			fprintf(stderr, "failed to adapt interrupt signal to epoll\n");
			cleanup(head, &epfd, &signal_fd);
		}

		while (true) {
			nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

			if (nfds == -1) {
				fprintf(stderr, "epoll failure\n");
				free_all_devices(head);
				exit(1);
			}

			for (n = 0; n < nfds; n++) {
				if (events[n].data.fd == signal_fd) {
					cleanup(head, &epfd, &signal_fd);
					exit(1);
				}


				if (events[n].data.ptr != NULL) {
					d = (struct Device *) events[n].data.ptr;
					rc = next_events(d, &options, &target, LIBEVDEV_READ_FLAG_NORMAL);

					if (rc != -EAGAIN && rc < 0) {
						fprintf(stderr, "failed next event processing with %d\n", rc);
					}
				}
			}
		}

		cleanup(head, &epfd, &signal_fd);
	}
}

void key_code_print_key_codes() {
	for (int i = 0; i < KEY_CODE_ARRAY_LENGTH; i++) {
		printf("%d: %s\n", key_codes[i].code, key_codes[i].key);
	}
}

int key_code_parse(unsigned int *code, char *name_or_code) {
	int rc = -1;
	char *endptr;

	long c = strtol(name_or_code, &endptr, 0);

	if (endptr != NULL || name_or_code == endptr) {
		for (int i = 0; i < KEY_CODE_ARRAY_LENGTH; i++) {
			if (strcmp(key_codes[i].key, name_or_code) == 0) {
				c = key_codes[i].code;
				rc = 0;
			}
		}
	} else {
		rc = 0;
	}

	*code = (unsigned int)c;
	
	return rc;
}

const struct KeyCode* key_code_by_code(unsigned int code) {
	for (int i = 0; i < KEY_CODE_ARRAY_LENGTH; i++) {
		if (key_codes[i].code == code) {
			return &key_codes[i];
		}
	}

	return NULL;
}

const struct KeyCode key_codes[] = {
	{ .key = "KEY_RESERVED", .code = KEY_RESERVED },
	{ .key = "KEY_ESC", .code = KEY_ESC },
	{ .key = "KEY_1", .code = KEY_1 },
	{ .key = "KEY_2", .code = KEY_2 },
	{ .key = "KEY_3", .code = KEY_3 },
	{ .key = "KEY_4", .code = KEY_4 },
	{ .key = "KEY_5", .code = KEY_5 },
	{ .key = "KEY_6", .code = KEY_6 },
	{ .key = "KEY_7", .code = KEY_7 },
	{ .key = "KEY_8", .code = KEY_8 },
	{ .key = "KEY_9", .code = KEY_9 },
	{ .key = "KEY_0", .code = KEY_0 },
	{ .key = "KEY_MINUS", .code = KEY_MINUS },
	{ .key = "KEY_EQUAL", .code = KEY_EQUAL },
	{ .key = "KEY_BACKSPACE", .code = KEY_BACKSPACE },
	{ .key = "KEY_TAB", .code = KEY_TAB },
	{ .key = "KEY_Q", .code = KEY_Q },
	{ .key = "KEY_W", .code = KEY_W },
	{ .key = "KEY_E", .code = KEY_E },
	{ .key = "KEY_R", .code = KEY_R },
	{ .key = "KEY_T", .code = KEY_T },
	{ .key = "KEY_Y", .code = KEY_Y },
	{ .key = "KEY_U", .code = KEY_U },
	{ .key = "KEY_I", .code = KEY_I },
	{ .key = "KEY_O", .code = KEY_O },
	{ .key = "KEY_P", .code = KEY_P },
	{ .key = "KEY_LEFTBRACE", .code = KEY_LEFTBRACE },
	{ .key = "KEY_RIGHTBRACE", .code = KEY_RIGHTBRACE },
	{ .key = "KEY_ENTER", .code = KEY_ENTER },
	{ .key = "KEY_LEFTCTRL", .code = KEY_LEFTCTRL },
	{ .key = "KEY_A", .code = KEY_A },
	{ .key = "KEY_S", .code = KEY_S },
	{ .key = "KEY_D", .code = KEY_D },
	{ .key = "KEY_F", .code = KEY_F },
	{ .key = "KEY_G", .code = KEY_G },
	{ .key = "KEY_H", .code = KEY_H },
	{ .key = "KEY_J", .code = KEY_J },
	{ .key = "KEY_K", .code = KEY_K },
	{ .key = "KEY_L", .code = KEY_L },
	{ .key = "KEY_SEMICOLON", .code = KEY_SEMICOLON },
	{ .key = "KEY_APOSTROPHE", .code = KEY_APOSTROPHE },
	{ .key = "KEY_GRAVE", .code = KEY_GRAVE },
	{ .key = "KEY_LEFTSHIFT", .code = KEY_LEFTSHIFT },
	{ .key = "KEY_BACKSLASH", .code = KEY_BACKSLASH },
	{ .key = "KEY_Z", .code = KEY_Z },
	{ .key = "KEY_X", .code = KEY_X },
	{ .key = "KEY_C", .code = KEY_C },
	{ .key = "KEY_V", .code = KEY_V },
	{ .key = "KEY_B", .code = KEY_B },
	{ .key = "KEY_N", .code = KEY_N },
	{ .key = "KEY_M", .code = KEY_M },
	{ .key = "KEY_COMMA", .code = KEY_COMMA },
	{ .key = "KEY_DOT", .code = KEY_DOT },
	{ .key = "KEY_SLASH", .code = KEY_SLASH },
	{ .key = "KEY_RIGHTSHIFT", .code = KEY_RIGHTSHIFT },
	{ .key = "KEY_KPASTERISK", .code = KEY_KPASTERISK },
	{ .key = "KEY_LEFTALT", .code = KEY_LEFTALT },
	{ .key = "KEY_SPACE", .code = KEY_SPACE },
	{ .key = "KEY_CAPSLOCK", .code = KEY_CAPSLOCK },
	{ .key = "KEY_F1", .code = KEY_F1 },
	{ .key = "KEY_F2", .code = KEY_F2 },
	{ .key = "KEY_F3", .code = KEY_F3 },
	{ .key = "KEY_F4", .code = KEY_F4 },
	{ .key = "KEY_F5", .code = KEY_F5 },
	{ .key = "KEY_F6", .code = KEY_F6 },
	{ .key = "KEY_F7", .code = KEY_F7 },
	{ .key = "KEY_F8", .code = KEY_F8 },
	{ .key = "KEY_F9", .code = KEY_F9 },
	{ .key = "KEY_F10", .code = KEY_F10 },
	{ .key = "KEY_NUMLOCK", .code = KEY_NUMLOCK },
	{ .key = "KEY_SCROLLLOCK", .code = KEY_SCROLLLOCK },
	{ .key = "KEY_KP7", .code = KEY_KP7 },
	{ .key = "KEY_KP8", .code = KEY_KP8 },
	{ .key = "KEY_KP9", .code = KEY_KP9 },
	{ .key = "KEY_KPMINUS", .code = KEY_KPMINUS },
	{ .key = "KEY_KP4", .code = KEY_KP4 },
	{ .key = "KEY_KP5", .code = KEY_KP5 },
	{ .key = "KEY_KP6", .code = KEY_KP6 },
	{ .key = "KEY_KPPLUS", .code = KEY_KPPLUS },
	{ .key = "KEY_KP1", .code = KEY_KP1 },
	{ .key = "KEY_KP2", .code = KEY_KP2 },
	{ .key = "KEY_KP3", .code = KEY_KP3 },
	{ .key = "KEY_KP0", .code = KEY_KP0 },
	{ .key = "KEY_KPDOT", .code = KEY_KPDOT },
	{ .key = "KEY_ZENKAKUHANKAKU", .code = KEY_ZENKAKUHANKAKU },
	{ .key = "KEY_102ND", .code = KEY_102ND },
	{ .key = "KEY_F11", .code = KEY_F11 },
	{ .key = "KEY_F12", .code = KEY_F12 },
	{ .key = "KEY_RO", .code = KEY_RO },
	{ .key = "KEY_KATAKANA", .code = KEY_KATAKANA },
	{ .key = "KEY_HIRAGANA", .code = KEY_HIRAGANA },
	{ .key = "KEY_HENKAN", .code = KEY_HENKAN },
	{ .key = "KEY_KATAKANAHIRAGANA", .code = KEY_KATAKANAHIRAGANA },
	{ .key = "KEY_MUHENKAN", .code = KEY_MUHENKAN },
	{ .key = "KEY_KPJPCOMMA", .code = KEY_KPJPCOMMA },
	{ .key = "KEY_KPENTER", .code = KEY_KPENTER },
	{ .key = "KEY_RIGHTCTRL", .code = KEY_RIGHTCTRL },
	{ .key = "KEY_KPSLASH", .code = KEY_KPSLASH },
	{ .key = "KEY_SYSRQ", .code = KEY_SYSRQ },
	{ .key = "KEY_RIGHTALT", .code = KEY_RIGHTALT },
	{ .key = "KEY_LINEFEED", .code = KEY_LINEFEED },
	{ .key = "KEY_HOME", .code = KEY_HOME },
	{ .key = "KEY_UP", .code = KEY_UP },
	{ .key = "KEY_PAGEUP", .code = KEY_PAGEUP },
	{ .key = "KEY_LEFT", .code = KEY_LEFT },
	{ .key = "KEY_RIGHT", .code = KEY_RIGHT },
	{ .key = "KEY_END", .code = KEY_END },
	{ .key = "KEY_DOWN", .code = KEY_DOWN },
	{ .key = "KEY_PAGEDOWN", .code = KEY_PAGEDOWN },
	{ .key = "KEY_INSERT", .code = KEY_INSERT },
	{ .key = "KEY_DELETE", .code = KEY_DELETE },
	{ .key = "KEY_MACRO", .code = KEY_MACRO },
	{ .key = "KEY_MUTE", .code = KEY_MUTE },
	{ .key = "KEY_VOLUMEDOWN", .code = KEY_VOLUMEDOWN },
	{ .key = "KEY_VOLUMEUP", .code = KEY_VOLUMEUP },
	{ .key = "KEY_POWER", .code = KEY_POWER },
	{ .key = "KEY_KPEQUAL", .code = KEY_KPEQUAL },
	{ .key = "KEY_KPPLUSMINUS", .code = KEY_KPPLUSMINUS },
	{ .key = "KEY_PAUSE", .code = KEY_PAUSE },
	{ .key = "KEY_SCALE", .code = KEY_SCALE },
	{ .key = "KEY_KPCOMMA", .code = KEY_KPCOMMA },
	{ .key = "KEY_HANGEUL", .code = KEY_HANGEUL },
	{ .key = "KEY_HANJA", .code = KEY_HANJA },
	{ .key = "KEY_YEN", .code = KEY_YEN },
	{ .key = "KEY_LEFTMETA", .code = KEY_LEFTMETA },
	{ .key = "KEY_RIGHTMETA", .code = KEY_RIGHTMETA },
	{ .key = "KEY_COMPOSE", .code = KEY_COMPOSE },
	{ .key = "KEY_STOP", .code = KEY_STOP },
	{ .key = "KEY_AGAIN", .code = KEY_AGAIN },
	{ .key = "KEY_PROPS", .code = KEY_PROPS },
	{ .key = "KEY_UNDO", .code = KEY_UNDO },
	{ .key = "KEY_FRONT", .code = KEY_FRONT },
	{ .key = "KEY_COPY", .code = KEY_COPY },
	{ .key = "KEY_OPEN", .code = KEY_OPEN },
	{ .key = "KEY_PASTE", .code = KEY_PASTE },
	{ .key = "KEY_FIND", .code = KEY_FIND },
	{ .key = "KEY_CUT", .code = KEY_CUT },
	{ .key = "KEY_HELP", .code = KEY_HELP },
	{ .key = "KEY_MENU", .code = KEY_MENU },
	{ .key = "KEY_CALC", .code = KEY_CALC },
	{ .key = "KEY_SETUP", .code = KEY_SETUP },
	{ .key = "KEY_SLEEP", .code = KEY_SLEEP },
	{ .key = "KEY_WAKEUP", .code = KEY_WAKEUP },
	{ .key = "KEY_FILE", .code = KEY_FILE },
	{ .key = "KEY_SENDFILE", .code = KEY_SENDFILE },
	{ .key = "KEY_DELETEFILE", .code = KEY_DELETEFILE },
	{ .key = "KEY_XFER", .code = KEY_XFER },
	{ .key = "KEY_PROG1", .code = KEY_PROG1 },
	{ .key = "KEY_PROG2", .code = KEY_PROG2 },
	{ .key = "KEY_WWW", .code = KEY_WWW },
	{ .key = "KEY_MSDOS", .code = KEY_MSDOS },
	{ .key = "KEY_COFFEE", .code = KEY_COFFEE },
	{ .key = "KEY_ROTATE_DISPLAY", .code = KEY_ROTATE_DISPLAY },
	{ .key = "KEY_CYCLEWINDOWS", .code = KEY_CYCLEWINDOWS },
	{ .key = "KEY_MAIL", .code = KEY_MAIL },
	{ .key = "KEY_BOOKMARKS", .code = KEY_BOOKMARKS },
	{ .key = "KEY_COMPUTER", .code = KEY_COMPUTER },
	{ .key = "KEY_BACK", .code = KEY_BACK },
	{ .key = "KEY_FORWARD", .code = KEY_FORWARD },
	{ .key = "KEY_CLOSECD", .code = KEY_CLOSECD },
	{ .key = "KEY_EJECTCD", .code = KEY_EJECTCD },
	{ .key = "KEY_EJECTCLOSECD", .code = KEY_EJECTCLOSECD },
	{ .key = "KEY_NEXTSONG", .code = KEY_NEXTSONG },
	{ .key = "KEY_PLAYPAUSE", .code = KEY_PLAYPAUSE },
	{ .key = "KEY_PREVIOUSSONG", .code = KEY_PREVIOUSSONG },
	{ .key = "KEY_STOPCD", .code = KEY_STOPCD },
	{ .key = "KEY_RECORD", .code = KEY_RECORD },
	{ .key = "KEY_REWIND", .code = KEY_REWIND },
	{ .key = "KEY_PHONE", .code = KEY_PHONE },
	{ .key = "KEY_ISO", .code = KEY_ISO },
	{ .key = "KEY_CONFIG", .code = KEY_CONFIG },
	{ .key = "KEY_HOMEPAGE", .code = KEY_HOMEPAGE },
	{ .key = "KEY_REFRESH", .code = KEY_REFRESH },
	{ .key = "KEY_EXIT", .code = KEY_EXIT },
	{ .key = "KEY_MOVE", .code = KEY_MOVE },
	{ .key = "KEY_EDIT", .code = KEY_EDIT },
	{ .key = "KEY_SCROLLUP", .code = KEY_SCROLLUP },
	{ .key = "KEY_SCROLLDOWN", .code = KEY_SCROLLDOWN },
	{ .key = "KEY_KPLEFTPAREN", .code = KEY_KPLEFTPAREN },
	{ .key = "KEY_KPRIGHTPAREN", .code = KEY_KPRIGHTPAREN },
	{ .key = "KEY_NEW", .code = KEY_NEW },
	{ .key = "KEY_REDO", .code = KEY_REDO },
	{ .key = "KEY_F13", .code = KEY_F13 },
	{ .key = "KEY_F14", .code = KEY_F14 },
	{ .key = "KEY_F15", .code = KEY_F15 },
	{ .key = "KEY_F16", .code = KEY_F16 },
	{ .key = "KEY_F17", .code = KEY_F17 },
	{ .key = "KEY_F18", .code = KEY_F18 },
	{ .key = "KEY_F19", .code = KEY_F19 },
	{ .key = "KEY_F20", .code = KEY_F20 },
	{ .key = "KEY_F21", .code = KEY_F21 },
	{ .key = "KEY_F22", .code = KEY_F22 },
	{ .key = "KEY_F23", .code = KEY_F23 },
	{ .key = "KEY_F24", .code = KEY_F24 },
	{ .key = "KEY_PLAYCD", .code = KEY_PLAYCD },
	{ .key = "KEY_PAUSECD", .code = KEY_PAUSECD },
	{ .key = "KEY_PROG3", .code = KEY_PROG3 },
	{ .key = "KEY_PROG4", .code = KEY_PROG4 },
	{ .key = "KEY_ALL_APPLICATIONS", .code = KEY_ALL_APPLICATIONS },
	{ .key = "KEY_SUSPEND", .code = KEY_SUSPEND },
	{ .key = "KEY_CLOSE", .code = KEY_CLOSE },
	{ .key = "KEY_PLAY", .code = KEY_PLAY },
	{ .key = "KEY_FASTFORWARD", .code = KEY_FASTFORWARD },
	{ .key = "KEY_BASSBOOST", .code = KEY_BASSBOOST },
	{ .key = "KEY_PRINT", .code = KEY_PRINT },
	{ .key = "KEY_HP", .code = KEY_HP },
	{ .key = "KEY_CAMERA", .code = KEY_CAMERA },
	{ .key = "KEY_SOUND", .code = KEY_SOUND },
	{ .key = "KEY_QUESTION", .code = KEY_QUESTION },
	{ .key = "KEY_EMAIL", .code = KEY_EMAIL },
	{ .key = "KEY_CHAT", .code = KEY_CHAT },
	{ .key = "KEY_SEARCH", .code = KEY_SEARCH },
	{ .key = "KEY_CONNECT", .code = KEY_CONNECT },
	{ .key = "KEY_FINANCE", .code = KEY_FINANCE },
	{ .key = "KEY_SPORT", .code = KEY_SPORT },
	{ .key = "KEY_SHOP", .code = KEY_SHOP },
	{ .key = "KEY_ALTERASE", .code = KEY_ALTERASE },
	{ .key = "KEY_CANCEL", .code = KEY_CANCEL },
	{ .key = "KEY_BRIGHTNESSDOWN", .code = KEY_BRIGHTNESSDOWN },
	{ .key = "KEY_BRIGHTNESSUP", .code = KEY_BRIGHTNESSUP },
	{ .key = "KEY_MEDIA", .code = KEY_MEDIA },
	{ .key = "KEY_SWITCHVIDEOMODE", .code = KEY_SWITCHVIDEOMODE },
	{ .key = "KEY_KBDILLUMTOGGLE", .code = KEY_KBDILLUMTOGGLE },
	{ .key = "KEY_KBDILLUMDOWN", .code = KEY_KBDILLUMDOWN },
	{ .key = "KEY_KBDILLUMUP", .code = KEY_KBDILLUMUP },
	{ .key = "KEY_SEND", .code = KEY_SEND },
	{ .key = "KEY_REPLY", .code = KEY_REPLY },
	{ .key = "KEY_FORWARDMAIL", .code = KEY_FORWARDMAIL },
	{ .key = "KEY_SAVE", .code = KEY_SAVE },
	{ .key = "KEY_DOCUMENTS", .code = KEY_DOCUMENTS },
	{ .key = "KEY_BATTERY", .code = KEY_BATTERY },
	{ .key = "KEY_BLUETOOTH", .code = KEY_BLUETOOTH },
	{ .key = "KEY_WLAN", .code = KEY_WLAN },
	{ .key = "KEY_UWB", .code = KEY_UWB },
	{ .key = "KEY_UNKNOWN", .code = KEY_UNKNOWN },
	{ .key = "KEY_VIDEO_NEXT", .code = KEY_VIDEO_NEXT },
	{ .key = "KEY_VIDEO_PREV", .code = KEY_VIDEO_PREV },
	{ .key = "KEY_BRIGHTNESS_CYCLE", .code = KEY_BRIGHTNESS_CYCLE },
	{ .key = "KEY_BRIGHTNESS_AUTO", .code = KEY_BRIGHTNESS_AUTO },
	{ .key = "KEY_DISPLAY_OFF", .code = KEY_DISPLAY_OFF },
	{ .key = "KEY_WWAN", .code = KEY_WWAN },
	{ .key = "KEY_RFKILL", .code = KEY_RFKILL },
	{ .key = "KEY_MICMUTE", .code = KEY_MICMUTE },
} ;

